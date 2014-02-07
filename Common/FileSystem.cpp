#include "FileSystem.h"
#include <stdexcept>
#include <stdio.h>
#include <string.h>


using namespace std;

static string ParentDirectory(const string& path)
{
	if (path.empty()) return path;

	size_t i = string::npos;

	if (*path.rbegin() == '/')
		i = path.length() - 2;

	i = path.rfind('/', i);

	if (i == string::npos)
		return "";

	// include the ending / in the result
	return path.substr(0, i+1);
}

#if WIN32
#define WIN32_LEAN_AND_MEAN
#include "windows.h"
#include "shlwapi.h"
#include <direct.h>
#include <algorithm>
#include <sys/types.h>
#include <sys/stat.h>

static inline void UTF8ToWide( const char* utf8, wchar_t* outBuffer, int outBufferSize )
{
	int res = MultiByteToWideChar( CP_UTF8, 0, utf8, -1, outBuffer, outBufferSize );
	if( res == 0 )
		outBuffer[0] = 0;
}

static inline void ConvertSeparatorsToWindows( wchar_t* pathName )
{
	while( *pathName != '\0' ) {
		if( *pathName == '/' )
			*pathName = '\\';
		++pathName;
	}
}

static inline void ConvertSeparatorsToLinux(string& pathName )
{
	replace(pathName.begin(), pathName.end(), '\\', '/');
}

void ConvertUnityPathName( const char* utf8, wchar_t* outBuffer, int outBufferSize )
{
	UTF8ToWide( utf8, outBuffer, outBufferSize );
	ConvertSeparatorsToWindows( outBuffer );
}

string PluginPath()
{
	HMODULE hModule = GetModuleHandleW(NULL);
	if (hModule == NULL)
		return "";

	CHAR path[MAX_PATH];
	if (GetModuleFileNameA(hModule, path, MAX_PATH) == 0)
		return "";
	return path;
}

bool EnsureDirectory(const string& path)
{
	string parent = ParentDirectory(path);
	if (!IsDirectory(parent) && !EnsureDirectory(parent))
		return false;

	wchar_t widePath[kDefaultPathBufferSize];
	ConvertUnityPathName(path.c_str(), widePath, kDefaultPathBufferSize);
	
	if (!PathFileExistsW(widePath))
	{
		// Create the path
		return 0 != CreateDirectoryW(widePath, NULL);
	} 
	else if (PathIsDirectoryW(widePath))
	{
		return true;
	}
	
	// Create the path
	return false;
	
}

static bool IsReadOnlyW(LPCWSTR path)
{
	DWORD attributes = GetFileAttributesW(path);
	// @TODO: Error handling
	/*
	if (INVALID_FILE_ATTRIBUTES == attributes)
		upipe.Log().Notice() << "Error stat on " << path << endl;
	 */
	return FILE_ATTRIBUTE_READONLY & attributes;
}

bool IsReadOnly(const string& path)
{
	wchar_t widePath[kDefaultPathBufferSize];
	ConvertUnityPathName(path.c_str(), widePath, kDefaultPathBufferSize);
	return PathExists(path) && IsReadOnlyW(widePath);
}

bool IsDirectory(const string& path)
{
	wchar_t widePath[kDefaultPathBufferSize];
	ConvertUnityPathName(path.c_str(), widePath, kDefaultPathBufferSize);
	return PathIsDirectoryW(widePath) != FALSE; // Must be compared against FALSE and not TRUE!
}

bool PathExists(const string& path)
{
	wchar_t widePath[kDefaultPathBufferSize];
	ConvertUnityPathName(path.c_str(), widePath, kDefaultPathBufferSize);
	return PathFileExistsW(widePath) == TRUE;	
}

bool ChangeCWD(const string& path)
{
	wchar_t widePath[kDefaultPathBufferSize];
	ConvertUnityPathName(path.c_str(), widePath, kDefaultPathBufferSize);
	return _wchdir(widePath) != -1;	
}

size_t GetFileLength(const string& pathName)
{
	wchar_t widePath[kDefaultPathBufferSize];
	ConvertUnityPathName(pathName.c_str(), widePath, kDefaultPathBufferSize);
	WIN32_FILE_ATTRIBUTE_DATA attrs;
	if (GetFileAttributesExW(widePath, GetFileExInfoStandard, &attrs) == 0)
	{
		throw runtime_error("Error getting file length attribute");
	}

	if (attrs.nFileSizeHigh)
		return UINT_MAX;
	return attrs.nFileSizeLow;
}

static bool RemoveReadOnlyW(LPCWSTR path)
{
	DWORD attributes = GetFileAttributesW(path);
	
	if (INVALID_FILE_ATTRIBUTES != attributes)
	{
		attributes &= ~FILE_ATTRIBUTE_READONLY;
		return 0 != SetFileAttributesW(path, attributes);
	}
	
	return false;
}

static bool RemoveDirectoryRecursiveWide( const wstring& path )
{
	if( path.empty() )
		return false;

	// base path
	wstring basePath = path;
	if( basePath[basePath.size()-1] != L'\\' )
		basePath += L'\\';

	// search pattern: anything inside the directory
	wstring searchPat = basePath + L'*';

	// find the first file
	WIN32_FIND_DATAW findData;
	HANDLE hFind = ::FindFirstFileW( searchPat.c_str(), &findData );
	if( hFind == INVALID_HANDLE_VALUE )
		return false;

	bool hadFailures = false;

	bool bSearch = true;
	while( bSearch )
	{
		if( ::FindNextFileW( hFind, &findData ) )
		{
			if( wcscmp(findData.cFileName,L".")==0 || wcscmp(findData.cFileName,L"..")==0 )
				continue;

			wstring filePath = basePath + findData.cFileName;
			if( (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) )
			{
				// we have found a directory, recurse
				if( !RemoveDirectoryRecursiveWide( filePath ) ) {
					hadFailures = true;
				} else {
					::RemoveDirectoryW( filePath.c_str() ); // remove the empty directory
				}
			}
			else
			{
				if( findData.dwFileAttributes & FILE_ATTRIBUTE_READONLY )
					::SetFileAttributesW( filePath.c_str(), FILE_ATTRIBUTE_NORMAL );
				if( !::DeleteFileW( filePath.c_str() ) ) {
					hadFailures = true;
				}
			}
		}
		else
		{
			if( ::GetLastError() == ERROR_NO_MORE_FILES )
			{
				bSearch = false; // no more files there
			}
			else
			{
				// some error occurred
				::FindClose( hFind );
				return false;
			}
		}
	}
	::FindClose( hFind );

	if( !RemoveDirectoryW( path.c_str() ) ) { // remove the empty directory
		hadFailures = true;
	}

	return !hadFailures;
}

static bool RemoveDirectoryRecursive( const string& pathUtf8 )
{
	wchar_t widePath[kDefaultPathBufferSize];
	ConvertUnityPathName( pathUtf8.c_str(), widePath, kDefaultPathBufferSize );
	return RemoveDirectoryRecursiveWide( widePath );
}

bool DeleteRecursive(const string& path)
{
	if( IsDirectory(path) )
		return RemoveDirectoryRecursive( path );
	else
	{
		wchar_t widePath[kDefaultPathBufferSize];
		ConvertUnityPathName(path.c_str(), widePath, kDefaultPathBufferSize);

		if (DeleteFileW(widePath))
		{
			return true;
		}

		#if UNITY_EDITOR

		if (ERROR_ACCESS_DENIED == GetLastError())
		{
			if (RemoveReadOnlyW(widePath))
			{
				return (FALSE != DeleteFileW(widePath));
			}
		}

		#endif

		return false;
	}
}

bool CopyAFile(const string& fromPath, const string& toPath, bool createMissingFolders)
{
	if (createMissingFolders && !EnsureDirectory(ParentDirectory(toPath)))
		return false;

	wchar_t wideFrom[kDefaultPathBufferSize], wideTo[kDefaultPathBufferSize];
	ConvertUnityPathName( fromPath.c_str(), wideFrom, kDefaultPathBufferSize );
	ConvertUnityPathName( toPath.c_str(), wideTo, kDefaultPathBufferSize );

	BOOL b = FALSE;
	if( CopyFileExW( wideFrom, wideTo, NULL, NULL, &b, 0) )
		return true;
	
	if (ERROR_ACCESS_DENIED == GetLastError())
	{
		if (RemoveReadOnlyW(wideTo))
		{
			b = FALSE;
			if ( CopyFileExW( wideFrom, wideTo, NULL, NULL, &b, 0) )
				return true;
		}
	}
	return false;
}

bool MoveAFile(const string& fromPath, const string& toPath)
{
	wchar_t wideFrom[kDefaultPathBufferSize], wideTo[kDefaultPathBufferSize];
	ConvertUnityPathName( fromPath.c_str(), wideFrom, kDefaultPathBufferSize );
	ConvertUnityPathName( toPath.c_str(), wideTo, kDefaultPathBufferSize );
	if( MoveFileExW( wideFrom, wideTo, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED ) )
		return true;
	
	if (ERROR_ACCESS_DENIED == GetLastError())
	{
		if (RemoveReadOnlyW(wideTo))
		{
			if (MoveFileExW(wideFrom, wideTo, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
			{
				return true;
			}
		}
	}
	return false;
}

bool ReadAFile(const string& path, string& data)
{
	wchar_t widePath[kDefaultPathBufferSize];
	ConvertUnityPathName( path.c_str(), widePath, kDefaultPathBufferSize );

	HANDLE handle = CreateFileW( widePath, GENERIC_READ, 0, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
	if (handle == NULL)
		return false;

	data.clear();
	DWORD bytesRead;
    char buffer[kDefaultBufferSize];
	BOOL success = ReadFile (handle, buffer, kDefaultBufferSize, &bytesRead, 0);
	while (success == TRUE && bytesRead > 0)
	{
		data += string(buffer, bytesRead);
		success = ReadFile (handle, buffer, kDefaultBufferSize, &bytesRead, 0);
	}

	CloseHandle( handle );
	return (success == TRUE);
}

bool WriteAFile(const string& path, const string& data)
{
	wchar_t widePath[kDefaultPathBufferSize];
	ConvertUnityPathName( path.c_str(), widePath, kDefaultPathBufferSize );
	
	HANDLE handle = CreateFileW( widePath, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
	if (handle == NULL)
		return false;

	DWORD bytesWritten;
	BOOL success = WriteFile (handle, data.c_str(), data.length(), &bytesWritten, 0);
	CloseHandle( handle );

	return (success == TRUE && bytesWritten == data.length());
}

static void FileSizeFromFindData(const WIN32_FIND_DATAW& findData, uint64_t& size)
{
    ULARGE_INTEGER fileSize;
    fileSize.HighPart = findData.nFileSizeHigh;
    fileSize.LowPart = findData.nFileSizeLow;
    size = (uint64_t)fileSize.QuadPart;
}

static const ULONGLONG kSecondsFromFileTimeToTimet = 11644473600;

static void TimeFromFileTime(const FILETIME& fileTime, time_t& time)
{
    ULARGE_INTEGER fTime;
    fTime.HighPart = fileTime.dwHighDateTime;
    fTime.LowPart = fileTime.dwLowDateTime;

    time = fTime.QuadPart / 10000000 - kSecondsFromFileTimeToTimet;
}

bool ScanDirectory(const string& path, bool recurse, FileCallBack cb, void *data)
{
	wchar_t widePath[kDefaultPathBufferSize];
	ConvertUnityPathName( path.c_str(), widePath, kDefaultPathBufferSize );

	// base path
	wstring basePath = widePath;
	if( basePath[basePath.size()-1] != L'\\' )
		basePath += L'\\';

	// search pattern: anything inside the directory
	wstring searchPat = basePath + L'*';

	// find the first file
	WIN32_FIND_DATAW findData;
	HANDLE hFind = ::FindFirstFileW( searchPat.c_str(), &findData );
	if( hFind == INVALID_HANDLE_VALUE )
		return false;

	bool bSearch = true;
	while( bSearch )
	{
		if( ::FindNextFileW( hFind, &findData ) )
		{
			if( wcscmp(findData.cFileName,L".")==0 || wcscmp(findData.cFileName,L"..")==0 )
				continue;

			wstring filePath = basePath + findData.cFileName;
			string fullPath( filePath.begin(), filePath.end() );

			bool isDir = ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == FILE_ATTRIBUTE_DIRECTORY);
			time_t ts = 0;
			TimeFromFileTime(findData.ftLastWriteTime, ts);
			if (ts == 0) TimeFromFileTime(findData.ftCreationTime, ts);
			if (ts == 0) TimeFromFileTime(findData.ftLastAccessTime, ts);
			
			uint64_t fileSize = 0;
			FileSizeFromFindData(findData, fileSize);

			ConvertSeparatorsToLinux(fullPath);
			if (cb(data, fullPath, fileSize, isDir, (time_t)ts) != 0)
				break;

			if( recurse && isDir )
			{
				// we have found a directory, recurse
				if (!ScanDirectory(fullPath, recurse, cb, data))
					return false;
			}
		}
		else
		{
			if( ::GetLastError() == ERROR_NO_MORE_FILES )
			{
				bSearch = false; // no more files there
			}
			else
			{
				// some error occurred
				::FindClose( hFind );
				return false;
			}
		}
	}
	::FindClose( hFind );
	return true;
}

static const ULONGLONG kSecondsFromTimetToFileTime = 116444736000000000;

static void TimetToFileTime( time_t t, LPFILETIME pft )
{
    LONGLONG ll = Int32x32To64(t, 10000000) + kSecondsFromTimetToFileTime;
    pft->dwLowDateTime = (DWORD) ll;
    pft->dwHighDateTime = ll >> 32;
}

bool TouchAFile(const std::string& path, time_t ts)
{
	wchar_t widePath[kDefaultPathBufferSize];
	ConvertUnityPathName( path.c_str(), widePath, kDefaultPathBufferSize );

	HANDLE handle = ::CreateFileW(widePath, GENERIC_READ | FILE_WRITE_ATTRIBUTES, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return false;

	FILETIME fileTime;
	TimetToFileTime(ts, &fileTime);
	BOOL res = ::SetFileTime(handle, NULL, NULL, &fileTime);
    ::CloseHandle(handle);

	return (res == TRUE);
}

bool GetAFileInfo(const std::string& path, uint64_t* size, bool* isDirectory, time_t* ts)
{
	wchar_t widePath[kDefaultPathBufferSize];
	ConvertUnityPathName( path.c_str(), widePath, kDefaultPathBufferSize );

	WIN32_FIND_DATAW findData;
    HANDLE handle = ::FindFirstFileW(widePath, &findData);
    if (handle == INVALID_HANDLE_VALUE)
        return false;

	bool isDir = ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == FILE_ATTRIBUTE_DIRECTORY);
	if (isDirectory) *isDirectory = isDir;
	if (size) 
	{
		uint64_t fileSize = 0;
		FileSizeFromFindData(findData, fileSize);
		*size = fileSize;
	}
	
	if (ts) 
	{
		time_t fileTs = 0;
		TimeFromFileTime(findData.ftLastWriteTime, fileTs);
		if (fileTs == 0) TimeFromFileTime(findData.ftCreationTime, fileTs);
		if (fileTs == 0) TimeFromFileTime(findData.ftLastAccessTime, fileTs);
		*ts = fileTs;
	}

    ::FindClose(handle);
    return true;
}

#else // MACOS

#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <utime.h>

bool IsReadOnly(const string& path)
{
	struct stat st;
	// @TODO: Error handling
	int res = stat(path.c_str(), &st);

	if (res == -1)
	{
		// Notice if file actually exists by we could not stat
		if (errno != ENOENT && errno != ENOTDIR)
		{
			char buf[1024];
			snprintf(buf, 1024, "Could not stat %s error : %s (%i)\n", path.c_str(), strerror(errno), errno);
			throw runtime_error(buf);
		}
		return false;
	}

	return !(st.st_mode & S_IWUSR);
}

bool IsDirectory(const string& path)
{
	struct stat status;
	if (stat(path.c_str(), &status) != 0)
		return false;
	return S_ISDIR(status.st_mode);
}

bool EnsureDirectory(const string& path)
{
	string parent = ParentDirectory(path);
	if (!IsDirectory(parent) && !EnsureDirectory(parent))
		return false;

	int res = mkdir(path.c_str(), 0777);
	if (res == -1 && errno != EEXIST)
	{
		char buf[1024];
		snprintf(buf, 1024, "EnsureDirectory error %s for: %s\n", strerror(errno), path.c_str());
		throw runtime_error(buf);
	}
	return true;
}

static int DeleteRecursiveHelper(const string& path)
{
	int res;

	struct stat status;
	res = lstat(path.c_str(), &status);
	if (res != 0)
		return res;

	if (S_ISDIR(status.st_mode) && !S_ISLNK(status.st_mode))
	{
		DIR *dirp = opendir (path.c_str());
		if (dirp == NULL)
			return -1;

		struct dirent *dp;
		while ( (dp = readdir(dirp)) )
		{
			if (strcmp(dp->d_name, ".") != 0 && strcmp(dp->d_name, "..") != 0)
			{
				string name = dp->d_name;
				res = DeleteRecursiveHelper(path + "/" + name);
				if (res != 0)
				{
					closedir(dirp);
					return res;
				}
			}
		}
		closedir(dirp);

		res = rmdir(path.c_str());
	}
	else
	{
		res = unlink(path.c_str());
	}

	return res;
}

bool DeleteRecursive(const string& path)
{
	return DeleteRecursiveHelper(path) == 0;
}

bool PathExists(const string& path)
{
	return access(path.c_str(), F_OK) == 0;
}

bool ChangeCWD(const string& path)
{
	return chdir(path.c_str()) != -1;
}

size_t GetFileLength(const string& pathName)
{
	struct stat statbuffer;
	if( stat(pathName.c_str(), &statbuffer) != 0 )
	{
		throw runtime_error("Error getting file length");
	}
	
	return statbuffer.st_size;
}

static bool fcopy(FILE *f1, FILE *f2)
{
    char            buffer[BUFSIZ];
    size_t          n;

    while ((n = fread(buffer, sizeof(char), sizeof(buffer), f1)) > 0)
    {
        if (fwrite(buffer, sizeof(char), n, f2) != n)
			return false;
    }
    return true;
}

bool CopyAFile(const string& fromPath, const string& toPath, bool createMissingFolders)
{
	if (createMissingFolders && !EnsureDirectory(ParentDirectory(toPath)))
		return false;

    FILE *fp1;
    FILE *fp2;

    if ((fp1 = fopen(fromPath.c_str(), "rb")) == 0)
		return false;

	// Make sure the file does not exist already
	int l = unlink(toPath.c_str());
	if (l != 0 && errno != ENOENT && errno != ENOTDIR)
		return false; 

    if ((fp2 = fopen(toPath.c_str(), "wb")) == 0)
	{
		fclose(fp1);
		return false;
	}

	bool res = fcopy(fp1, fp2);
	fclose(fp1);
	fclose(fp2);
	return res;
}

bool MoveAFile(const string& fromPath, const string& toPath)
{
	int res = rename(fromPath.c_str(), toPath.c_str());
	return !res;
}

bool ReadAFile(const string& path, string& data)
{
	FILE* fp;
	if ((fp = fopen(path.c_str(), "r")) == 0)
		return false;
	
	data.clear();
	char buffer[BUFSIZ];
    size_t n;
	
    while ((n = fread(buffer, sizeof(char), sizeof(buffer), fp)) > 0)
    {
		data.append(&buffer[0], n);
    }
	fclose(fp);
	return true;
}

bool WriteAFile(const string& path, const string& data)
{
	FILE* fp;
	if ((fp = fopen(path.c_str(), "w")) == 0)
		return false;
	
	bool res = (fwrite(&data[0], sizeof(char), data.length(), fp) == data.length());
	fflush(fp);
	fclose(fp);
	return res;
}

bool ScanDirectory(const string& path, bool recurse, FileCallBack cb, void *data)
{
	if (!IsDirectory(path))
		return false;

	DIR *dirp;
	struct dirent *dp;
	if ((dirp = opendir(path.c_str())) == NULL)
		return false;
    
	while ((dp = readdir(dirp)))
	{
        if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0)
            continue;
        
		string fullPath = path;
		if (path[path.length()-1] != '/') fullPath.append("/");
		fullPath.append(string(dp->d_name));
		
		struct stat statbuffer;
		stat(fullPath.c_str(), &statbuffer);

		bool isDir = S_ISDIR(statbuffer.st_mode);
		time_t ts = (time_t)statbuffer.st_mtime;
		if (ts == 0) ts = (time_t)statbuffer.st_ctime;
		if (ts == 0) ts = (time_t)statbuffer.st_atime;
		if (cb(data, fullPath, (double)statbuffer.st_size, isDir, (time_t)ts) != 0)
			break;

        if (isDir && recurse)
        {
            if (!ScanDirectory(fullPath, recurse, cb, data))
                return false;
        }
	}
	closedir(dirp);
    
	return true;
}

bool TouchAFile(const std::string& path, time_t ts)
{
	if (IsDirectory(path))
		return false;
	
	struct stat statbuffer;
	if (stat(path.c_str(), &statbuffer) != 0)
		return false;

	struct utimbuf utimebuffer;
	utimebuffer.actime = (time_t)statbuffer.st_atime;
	utimebuffer.modtime = ts;
	
	if (utime(path.c_str(), &utimebuffer) != 0)
		return false;
	
	return true;
}

bool GetAFileInfo(const std::string& path, uint64_t* size, bool* isDirectory, time_t* ts)
{
	struct stat statbuffer;
	if (stat(path.c_str(), &statbuffer) != 0)
		return false;

	bool isDir = S_ISDIR(statbuffer.st_mode);
	if (isDirectory) *isDirectory = isDir;
	if (size) *size = isDir ? 0 : (double)statbuffer.st_size;
	if (ts) {
		if (isDir)
			*ts = 0;
		else
		{
			*ts = (time_t)statbuffer.st_mtime;
			if (*ts == 0) *ts = (time_t)statbuffer.st_ctime;
			if (*ts == 0) *ts = (time_t)statbuffer.st_atime;
		}
	}
	
	return true;
}

#endif
