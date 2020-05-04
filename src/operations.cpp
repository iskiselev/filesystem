//  operations.cpp  --------------------------------------------------------------------//

//  Copyright 2002-2009, 2014 Beman Dawes
//  Copyright 2001 Dietmar Kuehl
//  Copyright 2018-2020 Andrey Semashev

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See library home page at http://www.boost.org/libs/filesystem

//--------------------------------------------------------------------------------------//

//  define 64-bit offset macros BEFORE including boost/config.hpp (see ticket #5355)
#if defined(__ANDROID__) && defined(__ANDROID_API__) && __ANDROID_API__ < 24
// Android fully supports 64-bit file offsets only for API 24 and above.
//
// Trying to define _FILE_OFFSET_BITS=64 for APIs below 24
// leads to compilation failure for one or another reason,
// depending on target Android API level, Android NDK version,
// used STL, order of include paths and more.
// For more information, please see:
// - https://github.com/boostorg/filesystem/issues/65
// - https://github.com/boostorg/filesystem/pull/69
//
// Android NDK developers consider it the expected behavior.
// See their official position here:
// - https://github.com/android-ndk/ndk/issues/501#issuecomment-326447479
// - https://android.googlesource.com/platform/bionic/+/a34817457feee026e8702a1d2dffe9e92b51d7d1/docs/32-bit-abi.md#32_bit-abi-bugs
//
// Thus we do not define _FILE_OFFSET_BITS in such case.
#else
// Defining _FILE_OFFSET_BITS=64 should kick in 64-bit off_t's
// (and thus st_size) on 32-bit systems that provide the Large File
// Support (LFS) interface, such as Linux, Solaris, and IRIX.
//
// At the time of this comment writing (March 2018), on most systems
// _FILE_OFFSET_BITS=64 definition is harmless:
// either the definition is supported and enables 64-bit off_t,
// or the definition is not supported and is ignored, in which case
// off_t does not change its default size for the target system
// (which may be 32-bit or 64-bit already).
// Thus it makes sense to have _FILE_OFFSET_BITS=64 defined by default,
// instead of listing every system that supports the definition.
// Those few systems, on which _FILE_OFFSET_BITS=64 is harmful,
// for example this definition causes compilation failure on those systems,
// should be exempt from defining _FILE_OFFSET_BITS by adding
// an appropriate #elif block above with the appropriate comment.
//
// _FILE_OFFSET_BITS must be defined before any headers are included
// to ensure that the definition is available to all included headers.
// That is required at least on Solaris, and possibly on other
// systems as well.
#define _FILE_OFFSET_BITS 64
#endif

#ifndef BOOST_SYSTEM_NO_DEPRECATED
# define BOOST_SYSTEM_NO_DEPRECATED
#endif

#ifndef _POSIX_PTHREAD_SEMANTICS
# define _POSIX_PTHREAD_SEMANTICS  // Sun readdir_r() needs this
#endif

// Include Boost.Predef first so that windows.h is guaranteed to be not included
#include <boost/predef/os/windows.h>
#if BOOST_OS_WINDOWS
#include <boost/winapi/config.hpp>
#endif

#include <boost/filesystem/config.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/file_status.hpp>
#include <boost/filesystem/exception.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/system/error_code.hpp>
#include <boost/smart_ptr/scoped_ptr.hpp>
#include <boost/smart_ptr/scoped_array.hpp>
#include <boost/detail/workaround.hpp>
#include <boost/cstdint.hpp>
#include <boost/assert.hpp>
#include <new> // std::bad_alloc, std::nothrow
#include <limits>
#include <string>
#include <cstddef>
#include <cstdlib>     // for malloc, free
#include <cstring>
#include <cstdio>      // for remove, rename
#if defined(__QNXNTO__)  // see ticket #5355
# include <stdio.h>
#endif
#include <cerrno>

#ifdef BOOST_FILEYSTEM_INCLUDE_IOSTREAM
# include <iostream>
#endif

# ifdef BOOST_POSIX_API

#   include <sys/types.h>
#   include <sys/stat.h>
#   if !defined(__APPLE__) && !defined(__OpenBSD__) && !defined(__ANDROID__) \
 && !defined(__VXWORKS__)
#     include <sys/statvfs.h>
#     define BOOST_STATVFS statvfs
#     define BOOST_STATVFS_F_FRSIZE vfs.f_frsize
#   else
#     ifdef __OpenBSD__
#       include <sys/param.h>
#     elif defined(__ANDROID__)
#       include <sys/vfs.h>
#     endif
#     if !defined(__VXWORKS__)
#       include <sys/mount.h>
#     endif
#     define BOOST_STATVFS statfs
#     define BOOST_STATVFS_F_FRSIZE static_cast<boost::uintmax_t>(vfs.f_bsize)
#   endif
#   include <unistd.h>
#   include <fcntl.h>
#   if _POSIX_C_SOURCE < 200809L
#     include <utime.h>
#   endif
#   include "limits.h"

# else // BOOST_WINDOWS_API

#   include <boost/winapi/dll.hpp> // get_proc_address, GetModuleHandleW
#   include <cwchar>
#   include <io.h>
#   include <windows.h>
#   include <winnt.h>
#   if defined(__BORLANDC__) || defined(__MWERKS__)
#     if defined(BOOST_BORLANDC)
        using std::time_t;
#     endif
#     include <utime.h>
#   else
#     include <sys/utime.h>
#   endif

#include "windows_tools.hpp"

# endif  // BOOST_WINDOWS_API

#include "error_handling.hpp"

namespace fs = boost::filesystem;
using boost::filesystem::path;
using boost::filesystem::filesystem_error;
using boost::filesystem::perms;
using boost::system::error_code;
using boost::system::system_category;

#if defined(BOOST_POSIX_API)

// At least Mac OS X 10.6 and older doesn't support O_CLOEXEC
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#if defined(_POSIX_SYNCHRONIZED_IO) && _POSIX_SYNCHRONIZED_IO > 0
#define BOOST_FILESYSTEM_HAS_FDATASYNC
#endif

#else // defined(BOOST_POSIX_API)

//  REPARSE_DATA_BUFFER related definitions are found in ntifs.h, which is part of the
//  Windows Device Driver Kit. Since that's inconvenient, the definitions are provided
//  here. See http://msdn.microsoft.com/en-us/library/ms791514.aspx

#if !defined(REPARSE_DATA_BUFFER_HEADER_SIZE)  // mingw winnt.h does provide the defs

#define SYMLINK_FLAG_RELATIVE 1

typedef struct _REPARSE_DATA_BUFFER {
  ULONG  ReparseTag;
  USHORT  ReparseDataLength;
  USHORT  Reserved;
  union {
    struct {
      USHORT  SubstituteNameOffset;
      USHORT  SubstituteNameLength;
      USHORT  PrintNameOffset;
      USHORT  PrintNameLength;
      ULONG  Flags;
      WCHAR  PathBuffer[1];
  /*  Example of distinction between substitute and print names:
        mklink /d ldrive c:\
        SubstituteName: c:\\??\
        PrintName: c:\
  */
     } SymbolicLinkReparseBuffer;
    struct {
      USHORT  SubstituteNameOffset;
      USHORT  SubstituteNameLength;
      USHORT  PrintNameOffset;
      USHORT  PrintNameLength;
      WCHAR  PathBuffer[1];
    } MountPointReparseBuffer;
    struct {
      UCHAR  DataBuffer[1];
    } GenericReparseBuffer;
  };
} REPARSE_DATA_BUFFER, *PREPARSE_DATA_BUFFER;

#define REPARSE_DATA_BUFFER_HEADER_SIZE \
  FIELD_OFFSET(REPARSE_DATA_BUFFER, GenericReparseBuffer)

#endif // !defined(REPARSE_DATA_BUFFER_HEADER_SIZE)

#ifndef MAXIMUM_REPARSE_DATA_BUFFER_SIZE
#define MAXIMUM_REPARSE_DATA_BUFFER_SIZE (16 * 1024)
#endif

#ifndef FSCTL_GET_REPARSE_POINT
#define FSCTL_GET_REPARSE_POINT 0x900a8
#endif

#ifndef IO_REPARSE_TAG_SYMLINK
#define IO_REPARSE_TAG_SYMLINK (0xA000000CL)
#endif

// Fallback for MinGW/Cygwin
#ifndef SYMBOLIC_LINK_FLAG_DIRECTORY
#define SYMBOLIC_LINK_FLAG_DIRECTORY 0x1
#endif

// Our convenience type for allocating REPARSE_DATA_BUFFER along with sufficient space after it
union reparse_data_buffer
{
  REPARSE_DATA_BUFFER rdb;
  unsigned char storage[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
};

# endif // defined(BOOST_POSIX_API)

//  POSIX/Windows macros  ----------------------------------------------------//

//  Portions of the POSIX and Windows API's are very similar, except for name,
//  order of arguments, and meaning of zero/non-zero returns. The macros below
//  abstract away those differences. They follow Windows naming and order of
//  arguments, and return true to indicate no error occurred. [POSIX naming,
//  order of arguments, and meaning of return were followed initially, but
//  found to be less clear and cause more coding errors.]

# if defined(BOOST_POSIX_API)

#   define BOOST_SET_CURRENT_DIRECTORY(P)(::chdir(P)== 0)
#   define BOOST_CREATE_DIRECTORY(P)(::mkdir(P, S_IRWXU|S_IRWXG|S_IRWXO)== 0)
#   define BOOST_CREATE_HARD_LINK(F,T)(::link(T, F)== 0)
#   define BOOST_CREATE_SYMBOLIC_LINK(F,T,Flag)(::symlink(T, F)== 0)
#   define BOOST_REMOVE_DIRECTORY(P)(::rmdir(P)== 0)
#   define BOOST_DELETE_FILE(P)(::unlink(P)== 0)
#   define BOOST_COPY_DIRECTORY(F,T)(!(::stat(from.c_str(), &from_stat)!= 0\
         || ::mkdir(to.c_str(),from_stat.st_mode)!= 0))
#   define BOOST_COPY_FILE(F,T,FailIfExistsBool)copy_file_api(F, T, FailIfExistsBool)
#   define BOOST_MOVE_FILE(OLD,NEW)(::rename(OLD, NEW)== 0)
#   define BOOST_RESIZE_FILE(P,SZ)(::truncate(P, SZ)== 0)

# else  // BOOST_WINDOWS_API

#   define BOOST_SET_CURRENT_DIRECTORY(P)(::SetCurrentDirectoryW(P)!= 0)
#   define BOOST_CREATE_DIRECTORY(P)(::CreateDirectoryW(P, 0)!= 0)
#   define BOOST_CREATE_HARD_LINK(F,T)(create_hard_link_api(F, T, 0)!= 0)
#   define BOOST_CREATE_SYMBOLIC_LINK(F,T,Flag)(create_symbolic_link_api(F, T, Flag)!= 0)
#   define BOOST_REMOVE_DIRECTORY(P)(::RemoveDirectoryW(P)!= 0)
#   define BOOST_DELETE_FILE(P)(::DeleteFileW(P)!= 0)
#   define BOOST_COPY_DIRECTORY(F,T)(::CreateDirectoryExW(F, T, 0)!= 0)
#   define BOOST_COPY_FILE(F,T,FailIfExistsBool)(::CopyFileW(F, T, FailIfExistsBool)!= 0)
#   define BOOST_MOVE_FILE(OLD,NEW)(::MoveFileExW(OLD, NEW, MOVEFILE_REPLACE_EXISTING|MOVEFILE_COPY_ALLOWED)!= 0)
#   define BOOST_RESIZE_FILE(P,SZ)(resize_file_api(P, SZ)!= 0)
#   define BOOST_READ_SYMLINK(P,T)

# endif

namespace boost {
namespace filesystem {
namespace detail {

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                        helpers (all operating systems)                               //
//                                                                                      //
//--------------------------------------------------------------------------------------//

namespace {

// Absolute maximum path length, in bytes, that we're willing to accept from various system calls.
// This value is arbitrary, it is supposed to be a hard limit to avoid memory exhaustion
// in some of the algorithms below in case of some corrupted or maliciously broken filesystem.
BOOST_CONSTEXPR_OR_CONST std::size_t absolute_path_max = 16u * 1024u * 1024u;

fs::file_type query_file_type(const path& p, error_code* ec);

//  general helpers  -----------------------------------------------------------------//

bool is_empty_directory(const path& p, error_code* ec)
{
  return (ec != 0 ? fs::directory_iterator(p, *ec) : fs::directory_iterator(p))
    == fs::directory_iterator();
}

bool not_found_error(int errval) BOOST_NOEXCEPT; // forward declaration

// only called if directory exists
bool remove_directory(const path& p) // true if succeeds or not found
{
  return BOOST_REMOVE_DIRECTORY(p.c_str())
    || not_found_error(BOOST_ERRNO);  // mitigate possible file system race. See #11166
}

// only called if file exists
bool remove_file(const path& p) // true if succeeds or not found
{
  return BOOST_DELETE_FILE(p.c_str())
    || not_found_error(BOOST_ERRNO);  // mitigate possible file system race. See #11166
}

// called by remove and remove_all_aux
bool remove_file_or_directory(const path& p, fs::file_type type, error_code* ec)
  // return true if file removed, false if not removed
{
  if (type == fs::file_not_found)
  {
    if (ec != 0) ec->clear();
    return false;
  }

  if (type == fs::directory_file
#     ifdef BOOST_WINDOWS_API
      || type == fs::_detail_directory_symlink
#     endif
    )
  {
    if (error(!remove_directory(p) ? BOOST_ERRNO : 0, p, ec,
      "boost::filesystem::remove"))
        return false;
  }
  else
  {
    if (error(!remove_file(p) ? BOOST_ERRNO : 0, p, ec,
      "boost::filesystem::remove"))
        return false;
  }
  return true;
}

boost::uintmax_t remove_all_aux(const path& p, fs::file_type type,
  error_code* ec)
{
  boost::uintmax_t count = 0;

  if (type == fs::directory_file)  // but not a directory symlink
  {
    fs::directory_iterator itr;
    if (ec != 0)
    {
      itr = fs::directory_iterator(p, *ec);
      if (*ec)
        return count;
    }
    else
      itr = fs::directory_iterator(p);

    const fs::directory_iterator end_dit;
    while(itr != end_dit)
    {
      fs::file_type tmp_type = query_file_type(itr->path(), ec);
      if (ec != 0 && *ec)
        return count;

      count += remove_all_aux(itr->path(), tmp_type, ec);
      if (ec != 0 && *ec)
        return count;

      fs::detail::directory_iterator_increment(itr, ec);
      if (ec != 0 && *ec)
        return count;
    }
  }

  remove_file_or_directory(p, type, ec);
  if (ec != 0 && *ec)
    return count;

  return ++count;
}

#ifdef BOOST_POSIX_API

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                            POSIX-specific helpers                                    //
//                                                                                      //
//--------------------------------------------------------------------------------------//

BOOST_CONSTEXPR_OR_CONST char dot = '.';

inline bool not_found_error(int errval) BOOST_NOEXCEPT
{
  return errval == ENOENT || errval == ENOTDIR;
}

//! Returns \c true of the two \c stat structures refer to the same file
inline bool equivalent_stat(struct ::stat const& s1, struct ::stat const& s2) BOOST_NOEXCEPT
{
  // According to the POSIX stat specs, "The st_ino and st_dev fields
  // taken together uniquely identify the file within the system."
  return s1.st_dev == s2.st_dev && s1.st_ino == s2.st_ino;
}

bool // true if ok
copy_file_api(const std::string& from_p,
  const std::string& to_p, bool fail_if_exists)
{
  int err = 0;

  int infile = ::open(from_p.c_str(), O_RDONLY | O_CLOEXEC);
  if (BOOST_UNLIKELY(infile < 0))
    return false;

  struct ::stat from_stat = {};
  if (BOOST_UNLIKELY(::fstat(infile, &from_stat) != 0))
  {
    err = errno;

  fail1:
    ::close(infile);
    errno = err;
    return false;
  }

  if (BOOST_UNLIKELY(!S_ISREG(from_stat.st_mode)))
  {
    err = ENOSYS;
    goto fail1;
  }

  // Enable writing for the newly created files. Having write permission set is important e.g. for NFS,
  // which checks the file permission on the server, even if the client's file descriptor supports writing.
  mode_t to_mode = from_stat.st_mode | S_IWUSR;

  int oflag = O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC;
  if (fail_if_exists)
    oflag |= O_EXCL;
  int outfile = ::open(to_p.c_str(), oflag, to_mode);
  if (BOOST_UNLIKELY(outfile < 0))
  {
    err = errno;
    goto fail1;
  }

  struct ::stat to_stat = {};
  if (BOOST_UNLIKELY(::fstat(outfile, &to_stat) != 0))
  {
    err = errno;

  fail2:
    ::close(outfile);
    ::close(infile);
    errno = err;
    return false;
  }

  if (BOOST_UNLIKELY(!S_ISREG(to_stat.st_mode)))
  {
    err = ENOSYS;
    goto fail2;
  }

  if (BOOST_UNLIKELY(equivalent_stat(from_stat, to_stat)))
  {
    err = EEXIST;
    goto fail2;
  }

  BOOST_CONSTEXPR_OR_CONST std::size_t buf_sz = 65536u;
  boost::scoped_array<char> buf(new (std::nothrow) char [buf_sz]);
  if (BOOST_UNLIKELY(!buf.get()))
  {
    err = ENOMEM;
    goto fail2;
  }

  while (true)
  {
    ssize_t sz_read = ::read(infile, buf.get(), buf_sz);
    if (sz_read == 0)
      break;
    if (BOOST_UNLIKELY(sz_read < 0))
    {
      err = errno;
      if (err == EINTR)
        continue;
      goto fail2;
    }

    // Allow for partial writes - see Advanced Unix Programming (2nd Ed.),
    // Marc Rochkind, Addison-Wesley, 2004, page 94
    for (ssize_t sz_wrote = 0; sz_wrote < sz_read;)
    {
      ssize_t sz = ::write(outfile, buf.get() + sz_wrote, static_cast< std::size_t >(sz_read - sz_wrote));
      if (BOOST_UNLIKELY(sz < 0))
      {
        err = errno;
        if (err == EINTR)
          continue;
        goto fail2;
      }

      sz_wrote += sz;
    }
  }

  // If we created a new file with an explicitly added S_IWUSR permission,
  // we may need to update its mode bits to match the source file.
  if (to_stat.st_mode != from_stat.st_mode)
  {
    if (BOOST_UNLIKELY(::fchmod(outfile, from_stat.st_mode) != 0))
    {
      err = errno;
      goto fail2;
    }
  }

  // Note: Use fsync/fdatasync followed by close to avoid dealing with the possibility of close failing with EINTR.
  // Even if close fails, including with EINTR, most operating systems (presumably, except HP-UX) will close the
  // file descriptor upon its return. This means that if an error happens later, when the OS flushes data to the
  // underlying media, this error will go unnoticed and we have no way to receive it from close. Calling fsync/fdatasync
  // ensures that all data have been written, and even if close fails for some unfathomable reason, we don't really
  // care at that point.
#if defined(BOOST_FILESYSTEM_HAS_FDATASYNC)
  err = ::fdatasync(outfile);
#else
  err = ::fsync(outfile);
#endif
  if (BOOST_UNLIKELY(err != 0))
  {
    err = errno;
    goto fail2;
  }

  ::close(outfile);
  ::close(infile);

  return true;
}

inline fs::file_type query_file_type(const path& p, error_code* ec)
{
  return fs::detail::symlink_status(p, ec).type();
}

# else

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                            Windows-specific helpers                                  //
//                                                                                      //
//--------------------------------------------------------------------------------------//

BOOST_CONSTEXPR_OR_CONST std::size_t buf_size = 128;

BOOST_CONSTEXPR_OR_CONST wchar_t dot = L'.';

inline std::wstring wgetenv(const wchar_t* name)
{
  // use a separate buffer since C++03 basic_string is not required to be contiguous
  const DWORD size = ::GetEnvironmentVariableW(name, NULL, 0);
  if (size > 0)
  {
    boost::scoped_array<wchar_t> buf(new wchar_t[size]);
    if (BOOST_LIKELY(::GetEnvironmentVariableW(name, buf.get(), size) > 0))
      return std::wstring(buf.get());
  }

  return std::wstring();
}

inline bool not_found_error(int errval) BOOST_NOEXCEPT
{
  return errval == ERROR_FILE_NOT_FOUND
    || errval == ERROR_PATH_NOT_FOUND
    || errval == ERROR_INVALID_NAME  // "tools/jam/src/:sys:stat.h", "//foo"
    || errval == ERROR_INVALID_DRIVE  // USB card reader with no card inserted
    || errval == ERROR_NOT_READY  // CD/DVD drive with no disc inserted
    || errval == ERROR_INVALID_PARAMETER  // ":sys:stat.h"
    || errval == ERROR_BAD_PATHNAME  // "//nosuch" on Win64
    || errval == ERROR_BAD_NETPATH;  // "//nosuch" on Win32
}

// these constants come from inspecting some Microsoft sample code
std::time_t to_time_t(const FILETIME & ft)
{
  __int64 t = (static_cast<__int64>(ft.dwHighDateTime)<< 32)
    + ft.dwLowDateTime;
#   if !defined(BOOST_MSVC) || BOOST_MSVC > 1300 // > VC++ 7.0
  t -= 116444736000000000LL;
#   else
  t -= 116444736000000000;
#   endif
  t /= 10000000;
  return static_cast<std::time_t>(t);
}

void to_FILETIME(std::time_t t, FILETIME & ft)
{
  __int64 temp = t;
  temp *= 10000000;
#   if !defined(BOOST_MSVC) || BOOST_MSVC > 1300 // > VC++ 7.0
  temp += 116444736000000000LL;
#   else
  temp += 116444736000000000;
#   endif
  ft.dwLowDateTime = static_cast<DWORD>(temp);
  ft.dwHighDateTime = static_cast<DWORD>(temp >> 32);
}

// Thanks to Jeremy Maitin-Shepard for much help and for permission to
// base the equivalent()implementation on portions of his
// file-equivalence-win32.cpp experimental code.

struct handle_wrapper
{
  HANDLE handle;
  handle_wrapper(HANDLE h)
    : handle(h){}
  ~handle_wrapper()
  {
    if (handle != INVALID_HANDLE_VALUE)
      ::CloseHandle(handle);
  }
};

HANDLE create_file_handle(const path& p, DWORD dwDesiredAccess,
  DWORD dwShareMode, LPSECURITY_ATTRIBUTES lpSecurityAttributes,
  DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes,
  HANDLE hTemplateFile)
{
  return ::CreateFileW(p.c_str(), dwDesiredAccess, dwShareMode,
    lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes,
    hTemplateFile);
}

bool is_reparse_point_a_symlink(const path& p)
{
  handle_wrapper h(create_file_handle(p, 0,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL));
  if (h.handle == INVALID_HANDLE_VALUE)
    return false;

  boost::scoped_ptr<reparse_data_buffer> buf(new reparse_data_buffer);

  // Query the reparse data
  DWORD dwRetLen = 0u;
  BOOL result = ::DeviceIoControl(h.handle, FSCTL_GET_REPARSE_POINT, NULL, 0, buf.get(),
    sizeof(*buf), &dwRetLen, NULL);
  if (!result) return false;

  return buf->rdb.ReparseTag == IO_REPARSE_TAG_SYMLINK
      // Issue 9016 asked that NTFS directory junctions be recognized as directories.
      // That is equivalent to recognizing them as symlinks, and then the normal symlink
      // mechanism will take care of recognizing them as directories.
      //
      // Directory junctions are very similar to symlinks, but have some performance
      // and other advantages over symlinks. They can be created from the command line
      // with "mklink /j junction-name target-path".
    || buf->rdb.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT;  // aka "directory junction" or "junction"
}

inline std::size_t get_full_path_name(
  const path& src, std::size_t len, wchar_t* buf, wchar_t** p)
{
  return static_cast<std::size_t>(
    ::GetFullPathNameW(src.c_str(), static_cast<DWORD>(len), buf, p));
}

fs::file_status process_status_failure(const path& p, error_code* ec)
{
  int errval(::GetLastError());
  if (ec != 0)                             // always report errval, even though some
    ec->assign(errval, system_category());   // errval values are not status_errors

  if (not_found_error(errval))
  {
    return fs::file_status(fs::file_not_found, fs::no_perms);
  }
  else if (errval == ERROR_SHARING_VIOLATION)
  {
    return fs::file_status(fs::type_unknown);
  }
  if (ec == 0)
    BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::status",
      p, error_code(errval, system_category())));
  return fs::file_status(fs::status_error);
}

//  differs from symlink_status() in that directory symlinks are reported as
//  _detail_directory_symlink, as required on Windows by remove() and its helpers.
fs::file_type query_file_type(const path& p, error_code* ec)
{
  DWORD attr(::GetFileAttributesW(p.c_str()));
  if (attr == 0xFFFFFFFF)
  {
    return process_status_failure(p, ec).type();
  }

  if (ec != 0) ec->clear();

  if (attr & FILE_ATTRIBUTE_REPARSE_POINT)
  {
    if (is_reparse_point_a_symlink(p))
      return (attr & FILE_ATTRIBUTE_DIRECTORY)
        ? fs::_detail_directory_symlink
        : fs::symlink_file;
    return fs::reparse_file;
  }

  return (attr & FILE_ATTRIBUTE_DIRECTORY)
    ? fs::directory_file
    : fs::regular_file;
}

BOOL resize_file_api(const wchar_t* p, boost::uintmax_t size)
{
  handle_wrapper h(CreateFileW(p, GENERIC_WRITE, 0, 0, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, 0));
  LARGE_INTEGER sz;
  sz.QuadPart = size;
  return h.handle != INVALID_HANDLE_VALUE
    && ::SetFilePointerEx(h.handle, sz, 0, FILE_BEGIN)
    && ::SetEndOfFile(h.handle);
}

//  Windows kernel32.dll functions that may or may not be present
//  must be accessed through pointers

typedef BOOL (WINAPI *PtrCreateHardLinkW)(
  /*__in*/       LPCWSTR lpFileName,
  /*__in*/       LPCWSTR lpExistingFileName,
  /*__reserved*/ LPSECURITY_ATTRIBUTES lpSecurityAttributes
 );

PtrCreateHardLinkW create_hard_link_api = PtrCreateHardLinkW(
  boost::winapi::get_proc_address(
    boost::winapi::GetModuleHandleW(L"kernel32.dll"), "CreateHardLinkW"));

typedef BOOLEAN (WINAPI *PtrCreateSymbolicLinkW)(
  /*__in*/ LPCWSTR lpSymlinkFileName,
  /*__in*/ LPCWSTR lpTargetFileName,
  /*__in*/ DWORD dwFlags
 );

PtrCreateSymbolicLinkW create_symbolic_link_api = PtrCreateSymbolicLinkW(
  boost::winapi::get_proc_address(
    boost::winapi::GetModuleHandleW(L"kernel32.dll"), "CreateSymbolicLinkW"));

#endif

//#ifdef BOOST_WINDOWS_API
//
//
//  inline bool get_free_disk_space(const std::wstring& ph,
//    PULARGE_INTEGER avail, PULARGE_INTEGER total, PULARGE_INTEGER free)
//    { return ::GetDiskFreeSpaceExW(ph.c_str(), avail, total, free)!= 0; }
//
//#endif

} // unnamed namespace
} // namespace detail

//--------------------------------------------------------------------------------------//
//                                                                                      //
//                operations functions declared in operations.hpp                       //
//                            in alphabetic order                                       //
//                                                                                      //
//--------------------------------------------------------------------------------------//

BOOST_FILESYSTEM_DECL
path absolute(const path& p, const path& base)
{
//  if ( p.empty() || p.is_absolute() )
//    return p;
//  //  recursively calling absolute is sub-optimal, but is simple
//  path abs_base(base.is_absolute() ? base : absolute(base));
//# ifdef BOOST_WINDOWS_API
//  if (p.has_root_directory())
//    return abs_base.root_name() / p;
//  //  !p.has_root_directory
//  if (p.has_root_name())
//    return p.root_name()
//      / abs_base.root_directory() / abs_base.relative_path() / p.relative_path();
//  //  !p.has_root_name()
//# endif
//  return abs_base / p;

  //  recursively calling absolute is sub-optimal, but is sure and simple
  path abs_base(base.is_absolute() ? base : absolute(base));

  //  store expensive to compute values that are needed multiple times
  path p_root_name (p.root_name());
  path base_root_name (abs_base.root_name());
  path p_root_directory (p.root_directory());

  if (p.empty())
    return abs_base;

  if (!p_root_name.empty())  // p.has_root_name()
  {
    if (p_root_directory.empty())  // !p.has_root_directory()
      return p_root_name / abs_base.root_directory()
      / abs_base.relative_path() / p.relative_path();
    // p is absolute, so fall through to return p at end of block
  }
  else if (!p_root_directory.empty())  // p.has_root_directory()
  {
#   ifdef BOOST_POSIX_API
    // POSIX can have root name it it is a network path
    if (base_root_name.empty())   // !abs_base.has_root_name()
      return p;
#   endif
    return base_root_name / p;
  }
  else
  {
    return abs_base / p;
  }

  return p;  // p.is_absolute() is true
}

namespace detail {

BOOST_FILESYSTEM_DECL bool possible_large_file_size_support()
{
# ifdef BOOST_POSIX_API
  typedef struct stat struct_stat;
  return sizeof(struct_stat().st_size) > 4;
# else
  return true;
# endif
}

BOOST_FILESYSTEM_DECL
path canonical(const path& p, const path& base, system::error_code* ec)
{
  path source (p.is_absolute() ? p : absolute(p, base));
  path root(source.root_path());
  path result;

  system::error_code local_ec;
  file_status stat (status(source, local_ec));

  if (stat.type() == fs::file_not_found)
  {
    if (ec == 0)
      BOOST_FILESYSTEM_THROW(filesystem_error(
        "boost::filesystem::canonical", source,
        error_code(system::errc::no_such_file_or_directory, system::generic_category())));
    ec->assign(system::errc::no_such_file_or_directory, system::generic_category());
    return result;
  }
  else if (local_ec)
  {
    if (ec == 0)
      BOOST_FILESYSTEM_THROW(filesystem_error(
        "boost::filesystem::canonical", source, local_ec));
    *ec = local_ec;
    return result;
  }

  bool scan = true;
  while (scan)
  {
    scan = false;
    result.clear();
    for (path::iterator itr = source.begin(); itr != source.end(); ++itr)
    {
      if (*itr == dot_path())
        continue;
      if (*itr == dot_dot_path())
      {
        if (result != root)
          result.remove_filename();
        continue;
      }

      result /= *itr;

      // If we don't have an absolute path yet then don't check symlink status.
      // This avoids checking "C:" which is "the current directory on drive C"
      // and hence not what we want to check/resolve here.
      if (!result.is_absolute())
          continue;

      bool is_sym (is_symlink(detail::symlink_status(result, ec)));
      if (ec && *ec)
        return path();

      if (is_sym)
      {
        path link(detail::read_symlink(result, ec));
        if (ec && *ec)
          return path();
        result.remove_filename();

        if (link.is_absolute())
        {
          for (++itr; itr != source.end(); ++itr)
            link /= *itr;
          source = link;
        }
        else // link is relative
        {
          path new_source(result);
          new_source /= link;
          for (++itr; itr != source.end(); ++itr)
            new_source /= *itr;
          source = new_source;
        }
        scan = true;  // symlink causes scan to be restarted
        break;
      }
    }
  }
  if (ec != 0)
    ec->clear();
  BOOST_ASSERT_MSG(result.is_absolute(), "canonical() implementation error; please report");
  return result;
}

BOOST_FILESYSTEM_DECL
void copy(const path& from, const path& to, system::error_code* ec)
{
  file_status s(detail::symlink_status(from, ec));
  if (ec != 0 && *ec) return;

  if (is_symlink(s))
  {
    detail::copy_symlink(from, to, ec);
  }
  else if (is_directory(s))
  {
    detail::copy_directory(from, to, ec);
  }
  else if (is_regular_file(s))
  {
    detail::copy_file(from, to, detail::fail_if_exists, ec);
  }
  else
  {
    if (ec == 0)
      BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::copy",
        from, to, error_code(BOOST_ERROR_NOT_SUPPORTED, system_category())));
    ec->assign(BOOST_ERROR_NOT_SUPPORTED, system_category());
  }
}

BOOST_FILESYSTEM_DECL
void copy_directory(const path& from, const path& to, system::error_code* ec)
{
# ifdef BOOST_POSIX_API
  struct stat from_stat;
# endif
  error(!BOOST_COPY_DIRECTORY(from.c_str(), to.c_str()) ? BOOST_ERRNO : 0,
    from, to, ec, "boost::filesystem::copy_directory");
}

BOOST_FILESYSTEM_DECL
void copy_file(const path& from, const path& to, copy_option option, error_code* ec)
{
  error(!BOOST_COPY_FILE(from.c_str(), to.c_str(),
    option == fail_if_exists) ? BOOST_ERRNO : 0,
      from, to, ec, "boost::filesystem::copy_file");
}

BOOST_FILESYSTEM_DECL
void copy_symlink(const path& existing_symlink, const path& new_symlink,
  system::error_code* ec)
{
  path p(read_symlink(existing_symlink, ec));
  if (ec != 0 && *ec) return;
  create_symlink(p, new_symlink, ec);
}

BOOST_FILESYSTEM_DECL
bool create_directories(const path& p, system::error_code* ec)
{
 if (p.empty())
 {
   if (ec == 0)
     BOOST_FILESYSTEM_THROW(filesystem_error(
       "boost::filesystem::create_directories", p,
       system::errc::make_error_code(system::errc::invalid_argument)));
   else
     ec->assign(system::errc::invalid_argument, system::generic_category());
   return false;
 }

  if (p.filename_is_dot() || p.filename_is_dot_dot())
    return create_directories(p.parent_path(), ec);

  error_code local_ec;
  file_status p_status = status(p, local_ec);

  if (p_status.type() == directory_file)
  {
    if (ec != 0)
      ec->clear();
    return false;
  }

  path parent = p.parent_path();
  BOOST_ASSERT_MSG(parent != p, "internal error: p == p.parent_path()");
  if (!parent.empty())
  {
    // determine if the parent exists
    file_status parent_status = status(parent, local_ec);

    // if the parent does not exist, create the parent
    if (parent_status.type() == file_not_found)
    {
      create_directories(parent, local_ec);
      if (local_ec)
      {
        if (ec == 0)
          BOOST_FILESYSTEM_THROW(filesystem_error(
            "boost::filesystem::create_directories", parent, local_ec));
        else
          *ec = local_ec;
        return false;
      }
    }
  }

  // create the directory
  return create_directory(p, ec);
}

BOOST_FILESYSTEM_DECL
bool create_directory(const path& p, error_code* ec)
{
  if (BOOST_CREATE_DIRECTORY(p.c_str()))
  {
    if (ec != 0)
      ec->clear();
    return true;
  }

  //  attempt to create directory failed
  int errval(BOOST_ERRNO);  // save reason for failure
  error_code dummy;

  if (is_directory(p, dummy))
  {
    if (ec != 0)
      ec->clear();
    return false;
  }

  //  attempt to create directory failed && it doesn't already exist
  if (ec == 0)
    BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::create_directory",
      p, error_code(errval, system_category())));
  else
    ec->assign(errval, system_category());

  return false;
}

BOOST_FILESYSTEM_DECL
void create_directory_symlink(const path& to, const path& from,
                               system::error_code* ec)
{
#if defined(BOOST_WINDOWS_API)
  // see if actually supported by Windows runtime dll
  if (error(!create_symbolic_link_api ? BOOST_ERROR_NOT_SUPPORTED : 0, to, from, ec,
      "boost::filesystem::create_directory_symlink"))
    return;
#endif

  error(!BOOST_CREATE_SYMBOLIC_LINK(from.c_str(), to.c_str(),
    SYMBOLIC_LINK_FLAG_DIRECTORY) ? BOOST_ERRNO : 0,
    to, from, ec, "boost::filesystem::create_directory_symlink");
}

BOOST_FILESYSTEM_DECL
void create_hard_link(const path& to, const path& from, error_code* ec)
{
#if defined(BOOST_WINDOWS_API)
  // see if actually supported by Windows runtime dll
  if (error(!create_hard_link_api ? BOOST_ERROR_NOT_SUPPORTED : 0, to, from, ec,
      "boost::filesystem::create_hard_link"))
    return;
#endif

  error(!BOOST_CREATE_HARD_LINK(from.c_str(), to.c_str()) ? BOOST_ERRNO : 0, to, from, ec,
    "boost::filesystem::create_hard_link");
}

BOOST_FILESYSTEM_DECL
void create_symlink(const path& to, const path& from, error_code* ec)
{
#if defined(BOOST_WINDOWS_API)
  // see if actually supported by Windows runtime dll
  if (error(!create_symbolic_link_api ? BOOST_ERROR_NOT_SUPPORTED : 0, to, from, ec,
      "boost::filesystem::create_symlink"))
    return;
#endif

  error(!BOOST_CREATE_SYMBOLIC_LINK(from.c_str(), to.c_str(), 0) ? BOOST_ERRNO : 0,
    to, from, ec, "boost::filesystem::create_symlink");
}

BOOST_FILESYSTEM_DECL
path current_path(error_code* ec)
{
# ifdef BOOST_POSIX_API
  struct local
  {
    static bool getcwd_error(error_code* ec)
    {
      const int err = errno;
      return error((err != ERANGE
          // bug in some versions of the Metrowerks C lib on the Mac: wrong errno set
#   if defined(__MSL__) && (defined(macintosh) || defined(__APPLE__) || defined(__APPLE_CC__))
          && err != 0
#   endif
        ) ? err : 0, ec, "boost::filesystem::current_path");
    }
  };

  path cur;
  char small_buf[1024];
  const char* p = ::getcwd(small_buf, sizeof(small_buf));
  if (BOOST_LIKELY(!!p))
  {
    cur = p;
    if (ec != 0) ec->clear();
  }
  else if (BOOST_LIKELY(!local::getcwd_error(ec)))
  {
    for (std::size_t path_max = sizeof(small_buf);; path_max *= 2u) // loop 'til buffer large enough
    {
      if (BOOST_UNLIKELY(path_max > absolute_path_max))
      {
        if (ec == 0)
          BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::current_path",
            error_code(ENAMETOOLONG, system_category())));
        else
          ec->assign(ENAMETOOLONG, system_category());
        break;
      }

      boost::scoped_array<char> buf(new char[path_max]);
      p = ::getcwd(buf.get(), path_max);
      if (BOOST_LIKELY(!!p))
      {
        cur = buf.get();
        if (ec != 0)
          ec->clear();
        break;
      }
      else if (BOOST_UNLIKELY(local::getcwd_error(ec)))
      {
        break;
      }
    }
  }

  return cur;

# elif defined(UNDER_CE)
  // Windows CE has no current directory, so everything's relative to the root of the directory tree
  return L"\\";
# else
  DWORD sz;
  if ((sz = ::GetCurrentDirectoryW(0, NULL)) == 0)sz = 1;
  boost::scoped_array<path::value_type> buf(new path::value_type[sz]);
  error(::GetCurrentDirectoryW(sz, buf.get()) == 0 ? BOOST_ERRNO : 0, ec,
    "boost::filesystem::current_path");
  return path(buf.get());
# endif
}


BOOST_FILESYSTEM_DECL
void current_path(const path& p, system::error_code* ec)
{
# ifdef UNDER_CE
  error(BOOST_ERROR_NOT_SUPPORTED, p, ec,
    "boost::filesystem::current_path");
# else
  error(!BOOST_SET_CURRENT_DIRECTORY(p.c_str()) ? BOOST_ERRNO : 0,
    p, ec, "boost::filesystem::current_path");
# endif
}

BOOST_FILESYSTEM_DECL
bool equivalent(const path& p1, const path& p2, system::error_code* ec)
{
# ifdef BOOST_POSIX_API
  // p2 is done first, so any error reported is for p1
  struct ::stat s2 = {};
  int e2 = ::stat(p2.c_str(), &s2);
  struct ::stat s1 = {};
  int e1 = ::stat(p1.c_str(), &s1);

  if (BOOST_UNLIKELY(e1 != 0 || e2 != 0))
  {
    // if one is invalid and the other isn't then they aren't equivalent,
    // but if both are invalid then it is an error
    if (e1 != 0 && e2 != 0)
      error(errno, p1, p2, ec, "boost::filesystem::equivalent");
    return false;
  }

  return equivalent_stat(s1, s2);

# else  // Windows

  // Note well: Physical location on external media is part of the
  // equivalence criteria. If there are no open handles, physical location
  // can change due to defragmentation or other relocations. Thus handles
  // must be held open until location information for both paths has
  // been retrieved.

  // p2 is done first, so any error reported is for p1
  handle_wrapper h2(
    create_file_handle(
        p2.c_str(),
        0,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
        0,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        0));

  handle_wrapper h1(
    create_file_handle(
        p1.c_str(),
        0,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
        0,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        0));

  if (BOOST_UNLIKELY(h1.handle == INVALID_HANDLE_VALUE || h2.handle == INVALID_HANDLE_VALUE))
  {
    // if one is invalid and the other isn't, then they aren't equivalent,
    // but if both are invalid then it is an error
    if (h1.handle == INVALID_HANDLE_VALUE && h2.handle == INVALID_HANDLE_VALUE)
      error(BOOST_ERRNO, p1, p2, ec, "boost::filesystem::equivalent");
    return false;
  }

  // at this point, both handles are known to be valid

  BY_HANDLE_FILE_INFORMATION info1, info2;

  if (error(!::GetFileInformationByHandle(h1.handle, &info1) ? BOOST_ERRNO : 0,
    p1, p2, ec, "boost::filesystem::equivalent"))
    return  false;

  if (error(!::GetFileInformationByHandle(h2.handle, &info2) ? BOOST_ERRNO : 0,
    p1, p2, ec, "boost::filesystem::equivalent"))
    return  false;

  // In theory, volume serial numbers are sufficient to distinguish between
  // devices, but in practice VSN's are sometimes duplicated, so last write
  // time and file size are also checked.
  return
    info1.dwVolumeSerialNumber == info2.dwVolumeSerialNumber
    && info1.nFileIndexHigh == info2.nFileIndexHigh
    && info1.nFileIndexLow == info2.nFileIndexLow
    && info1.nFileSizeHigh == info2.nFileSizeHigh
    && info1.nFileSizeLow == info2.nFileSizeLow
    && info1.ftLastWriteTime.dwLowDateTime
      == info2.ftLastWriteTime.dwLowDateTime
    && info1.ftLastWriteTime.dwHighDateTime
      == info2.ftLastWriteTime.dwHighDateTime;

# endif
}

BOOST_FILESYSTEM_DECL
boost::uintmax_t file_size(const path& p, error_code* ec)
{
# ifdef BOOST_POSIX_API

  struct ::stat path_stat;
  if (error(::stat(p.c_str(), &path_stat)!= 0 ? BOOST_ERRNO : 0,
      p, ec, "boost::filesystem::file_size"))
    return static_cast<boost::uintmax_t>(-1);
  if (error(!S_ISREG(path_stat.st_mode) ? EPERM : 0,
      p, ec, "boost::filesystem::file_size"))
    return static_cast<boost::uintmax_t>(-1);

  return static_cast<boost::uintmax_t>(path_stat.st_size);

# else  // Windows

  // assume uintmax_t is 64-bits on all Windows compilers

  WIN32_FILE_ATTRIBUTE_DATA fad;

  if (error(::GetFileAttributesExW(p.c_str(), ::GetFileExInfoStandard, &fad)== 0
    ? BOOST_ERRNO : 0, p, ec, "boost::filesystem::file_size"))
        return static_cast<boost::uintmax_t>(-1);

  if (error((fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)!= 0
    ? ERROR_NOT_SUPPORTED : 0, p, ec, "boost::filesystem::file_size"))
    return static_cast<boost::uintmax_t>(-1);

  return (static_cast<boost::uintmax_t>(fad.nFileSizeHigh)
            << (sizeof(fad.nFileSizeLow)*8)) + fad.nFileSizeLow;
# endif
}

BOOST_FILESYSTEM_DECL
boost::uintmax_t hard_link_count(const path& p, system::error_code* ec)
{
# ifdef BOOST_POSIX_API

  struct ::stat path_stat;
  return error(::stat(p.c_str(), &path_stat)!= 0 ? BOOST_ERRNO : 0,
                p, ec, "boost::filesystem::hard_link_count")
         ? 0
         : static_cast<boost::uintmax_t>(path_stat.st_nlink);

# else // Windows

  // Link count info is only available through GetFileInformationByHandle
  BY_HANDLE_FILE_INFORMATION info;
  handle_wrapper h(
    create_file_handle(p.c_str(), 0,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0));
  return
    !error(h.handle == INVALID_HANDLE_VALUE ? BOOST_ERRNO : 0,
            p, ec, "boost::filesystem::hard_link_count")
    && !error(::GetFileInformationByHandle(h.handle, &info)== 0 ? BOOST_ERRNO : 0,
               p, ec, "boost::filesystem::hard_link_count")
         ? info.nNumberOfLinks
         : 0;
# endif
}

BOOST_FILESYSTEM_DECL
path initial_path(error_code* ec)
{
  static path init_path;
  if (init_path.empty())
    init_path = current_path(ec);
  else if (ec != 0) ec->clear();
  return init_path;
}

BOOST_FILESYSTEM_DECL
bool is_empty(const path& p, system::error_code* ec)
{
# ifdef BOOST_POSIX_API

  struct ::stat path_stat;
  if (error(::stat(p.c_str(), &path_stat)!= 0,
      p, ec, "boost::filesystem::is_empty"))
    return false;
  return S_ISDIR(path_stat.st_mode)
    ? is_empty_directory(p, ec)
    : path_stat.st_size == 0;

# else

  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (error(::GetFileAttributesExW(p.c_str(), ::GetFileExInfoStandard, &fad)== 0
    ? BOOST_ERRNO : 0, p, ec, "boost::filesystem::is_empty"))
      return false;

  if (ec != 0) ec->clear();
  return
    (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      ? is_empty_directory(p, ec)
      : (!fad.nFileSizeHigh && !fad.nFileSizeLow);

# endif
}

BOOST_FILESYSTEM_DECL
std::time_t last_write_time(const path& p, system::error_code* ec)
{
# ifdef BOOST_POSIX_API

  struct ::stat path_stat;
  if (error(::stat(p.c_str(), &path_stat)!= 0 ? BOOST_ERRNO : 0,
    p, ec, "boost::filesystem::last_write_time"))
      return std::time_t(-1);
  return path_stat.st_mtime;

# else

  handle_wrapper hw(
    create_file_handle(p.c_str(), 0,
      FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0));

  if (error(hw.handle == INVALID_HANDLE_VALUE ? BOOST_ERRNO : 0,
    p, ec, "boost::filesystem::last_write_time"))
      return std::time_t(-1);

  FILETIME lwt;

  if (error(::GetFileTime(hw.handle, 0, 0, &lwt)== 0 ? BOOST_ERRNO : 0,
    p, ec, "boost::filesystem::last_write_time"))
      return std::time_t(-1);

  return to_time_t(lwt);

# endif
}

BOOST_FILESYSTEM_DECL
void last_write_time(const path& p, const std::time_t new_time,
                      system::error_code* ec)
{
# ifdef BOOST_POSIX_API
#   if _POSIX_C_SOURCE >= 200809L

  struct timespec times[2] = {};

  // Keep the last access time unchanged
  times[0].tv_nsec = UTIME_OMIT;

  times[1].tv_sec = new_time;

  if (BOOST_UNLIKELY(::utimensat(AT_FDCWD, p.c_str(), times, 0) != 0))
  {
    error(BOOST_ERRNO, p, ec, "boost::filesystem::last_write_time");
    return;
  }

#   else // _POSIX_C_SOURCE >= 200809L

  struct ::stat path_stat;
  if (error(::stat(p.c_str(), &path_stat)!= 0,
    p, ec, "boost::filesystem::last_write_time"))
      return;
  ::utimbuf buf;
  buf.actime = path_stat.st_atime; // utime()updates access time too:-(
  buf.modtime = new_time;
  error(::utime(p.c_str(), &buf)!= 0 ? BOOST_ERRNO : 0,
    p, ec, "boost::filesystem::last_write_time");

#   endif // _POSIX_C_SOURCE >= 200809L

# else

  handle_wrapper hw(
    create_file_handle(p.c_str(), FILE_WRITE_ATTRIBUTES,
      FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0));

  if (error(hw.handle == INVALID_HANDLE_VALUE ? BOOST_ERRNO : 0,
    p, ec, "boost::filesystem::last_write_time"))
      return;

  FILETIME lwt;
  to_FILETIME(new_time, lwt);

  error(::SetFileTime(hw.handle, 0, 0, &lwt)== 0 ? BOOST_ERRNO : 0,
    p, ec, "boost::filesystem::last_write_time");

# endif
}

# ifdef BOOST_POSIX_API
const perms active_bits(all_all | set_uid_on_exe | set_gid_on_exe | sticky_bit);
inline mode_t mode_cast(perms prms) { return prms & active_bits; }
# endif

BOOST_FILESYSTEM_DECL
void permissions(const path& p, perms prms, system::error_code* ec)
{
  BOOST_ASSERT_MSG(!((prms & add_perms) && (prms & remove_perms)),
    "add_perms and remove_perms are mutually exclusive");

  if ((prms & add_perms) && (prms & remove_perms))  // precondition failed
    return;

# ifdef BOOST_POSIX_API
  error_code local_ec;
  file_status current_status((prms & symlink_perms)
                             ? fs::symlink_status(p, local_ec)
                             : fs::status(p, local_ec));
  if (local_ec)
  {
    if (ec == 0)
    BOOST_FILESYSTEM_THROW(filesystem_error(
        "boost::filesystem::permissions", p, local_ec));
    else
      *ec = local_ec;
    return;
  }

  if (prms & add_perms)
    prms |= current_status.permissions();
  else if (prms & remove_perms)
    prms = current_status.permissions() & ~prms;

  // OS X <10.10, iOS <8.0 and some other platforms don't support fchmodat().
  // Solaris (SunPro and gcc) only support fchmodat() on Solaris 11 and higher,
  // and a runtime check is too much trouble.
  // Linux does not support permissions on symbolic links and has no plans to
  // support them in the future.  The chmod() code is thus more practical,
  // rather than always hitting ENOTSUP when sending in AT_SYMLINK_NO_FOLLOW.
  //  - See the 3rd paragraph of
  // "Symbolic link ownership, permissions, and timestamps" at:
  //   "http://man7.org/linux/man-pages/man7/symlink.7.html"
  //  - See the fchmodat() Linux man page:
  //   "http://man7.org/linux/man-pages/man2/fchmodat.2.html"
# if defined(AT_FDCWD) && defined(AT_SYMLINK_NOFOLLOW) \
    && !(defined(__SUNPRO_CC) || defined(__sun) || defined(sun)) \
    && !(defined(linux) || defined(__linux) || defined(__linux__)) \
    && !(defined(__MAC_OS_X_VERSION_MIN_REQUIRED) \
         && __MAC_OS_X_VERSION_MIN_REQUIRED < 101000) \
    && !(defined(__IPHONE_OS_VERSION_MIN_REQUIRED) \
         && __IPHONE_OS_VERSION_MIN_REQUIRED < 80000) \
    && !(defined(__QNX__) && (_NTO_VERSION <= 700))
  if (::fchmodat(AT_FDCWD, p.c_str(), mode_cast(prms),
       !(prms & symlink_perms) ? 0 : AT_SYMLINK_NOFOLLOW))
# else  // fallback if fchmodat() not supported
  if (::chmod(p.c_str(), mode_cast(prms)))
# endif
  {
    const int err = errno;
    if (ec == 0)
      BOOST_FILESYSTEM_THROW(filesystem_error(
        "boost::filesystem::permissions", p,
        error_code(err, system::generic_category())));
    else
      ec->assign(err, system::generic_category());
  }

# else  // Windows

  // if not going to alter FILE_ATTRIBUTE_READONLY, just return
  if (!(!((prms & (add_perms | remove_perms)))
    || (prms & (owner_write|group_write|others_write))))
    return;

  DWORD attr = ::GetFileAttributesW(p.c_str());

  if (error(attr == 0 ? BOOST_ERRNO : 0, p, ec, "boost::filesystem::permissions"))
    return;

  if (prms & add_perms)
    attr &= ~FILE_ATTRIBUTE_READONLY;
  else if (prms & remove_perms)
    attr |= FILE_ATTRIBUTE_READONLY;
  else if (prms & (owner_write|group_write|others_write))
    attr &= ~FILE_ATTRIBUTE_READONLY;
  else
    attr |= FILE_ATTRIBUTE_READONLY;

  error(::SetFileAttributesW(p.c_str(), attr) == 0 ? BOOST_ERRNO : 0,
    p, ec, "boost::filesystem::permissions");
# endif
}

BOOST_FILESYSTEM_DECL
path read_symlink(const path& p, system::error_code* ec)
{
  path symlink_path;

# ifdef BOOST_POSIX_API
  const char* const path_str = p.c_str();
  char small_buf[1024];
  ssize_t result = ::readlink(path_str, small_buf, sizeof(small_buf));
  if (BOOST_UNLIKELY(result < 0))
  {
  fail:
    const int err = errno;
    if (ec == 0)
      BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::read_symlink",
        p, error_code(err, system_category())));
    else
      ec->assign(err, system_category());
  }
  else if (BOOST_LIKELY(static_cast< std::size_t >(result) < sizeof(small_buf)))
  {
    symlink_path.assign(small_buf, small_buf + result);
    if (ec != 0)
      ec->clear();
  }
  else
  {
    for (std::size_t path_max = sizeof(small_buf) * 2u;; path_max *= 2u) // loop 'til buffer large enough
    {
      if (BOOST_UNLIKELY(path_max > absolute_path_max))
      {
        if (ec == 0)
          BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::read_symlink",
            p, error_code(ENAMETOOLONG, system_category())));
        else
          ec->assign(ENAMETOOLONG, system_category());
        break;
      }

      boost::scoped_array<char> buf(new char[path_max]);
      result = ::readlink(path_str, buf.get(), path_max);
      if (BOOST_UNLIKELY(result < 0))
      {
        goto fail;
      }
      else if (BOOST_LIKELY(static_cast< std::size_t >(result) < path_max))
      {
        symlink_path.assign(buf.get(), buf.get() + result);
        if (ec != 0) ec->clear();
        break;
      }
    }
  }

# else

  handle_wrapper h(
    create_file_handle(p.c_str(), 0,
      FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, 0));

  if (error(h.handle == INVALID_HANDLE_VALUE ? BOOST_ERRNO : 0,
    p, ec, "boost::filesystem::read_symlink"))
      return symlink_path;

  boost::scoped_ptr<reparse_data_buffer> buf(new reparse_data_buffer);
  DWORD sz = 0u;
  if (!error(::DeviceIoControl(h.handle, FSCTL_GET_REPARSE_POINT,
        0, 0, buf.get(), sizeof(*buf), &sz, 0) == 0 ? BOOST_ERRNO : 0, p, ec,
        "boost::filesystem::read_symlink" ))
  {
    const wchar_t* buffer;
    std::size_t offset, len;
    switch (buf->rdb.ReparseTag)
    {
    case IO_REPARSE_TAG_MOUNT_POINT:
      buffer = buf->rdb.MountPointReparseBuffer.PathBuffer;
      offset = buf->rdb.MountPointReparseBuffer.PrintNameOffset;
      len = buf->rdb.MountPointReparseBuffer.PrintNameLength;
      break;
    case IO_REPARSE_TAG_SYMLINK:
      buffer = buf->rdb.SymbolicLinkReparseBuffer.PathBuffer;
      offset = buf->rdb.SymbolicLinkReparseBuffer.PrintNameOffset;
      len = buf->rdb.SymbolicLinkReparseBuffer.PrintNameLength;
      // Note: iff info.rdb.SymbolicLinkReparseBuffer.Flags & SYMLINK_FLAG_RELATIVE
      //       -> resulting path is relative to the source
      break;
    default:
      error(BOOST_ERROR_NOT_SUPPORTED, p, ec, "Unknown ReparseTag in boost::filesystem::read_symlink");
      return symlink_path;
    }
    symlink_path.assign(
      buffer + offset / sizeof(wchar_t),
      buffer + (offset + len) / sizeof(wchar_t));
  }
# endif
  return symlink_path;
}

BOOST_FILESYSTEM_DECL
path relative(const path& p, const path& base, error_code* ec)
{
  error_code tmp_ec;
  path wc_base(weakly_canonical(base, &tmp_ec));
  if (error(tmp_ec.value(), base, ec, "boost::filesystem::relative"))
    return path();
  path wc_p(weakly_canonical(p, &tmp_ec));
  if (error(tmp_ec.value(), base, ec, "boost::filesystem::relative"))
    return path();
  return wc_p.lexically_relative(wc_base);
}

BOOST_FILESYSTEM_DECL
bool remove(const path& p, error_code* ec)
{
  error_code tmp_ec;
  file_type type = query_file_type(p, &tmp_ec);
  if (error(type == status_error ? tmp_ec.value() : 0, p, ec,
      "boost::filesystem::remove"))
    return false;

  // Since POSIX remove() is specified to work with either files or directories, in a
  // perfect world it could just be called. But some important real-world operating
  // systems (Windows, Mac OS X, for example) don't implement the POSIX spec. So
  // remove_file_or_directory() is always called to keep it simple.
  return remove_file_or_directory(p, type, ec);
}

BOOST_FILESYSTEM_DECL
boost::uintmax_t remove_all(const path& p, error_code* ec)
{
  error_code tmp_ec;
  file_type type = query_file_type(p, &tmp_ec);
  if (error(type == status_error ? tmp_ec.value() : 0, p, ec,
    "boost::filesystem::remove_all"))
    return 0;

  return (type != status_error && type != file_not_found) // exists
    ? remove_all_aux(p, type, ec)
    : 0;
}

BOOST_FILESYSTEM_DECL
void rename(const path& old_p, const path& new_p, error_code* ec)
{
  error(!BOOST_MOVE_FILE(old_p.c_str(), new_p.c_str()) ? BOOST_ERRNO : 0, old_p, new_p,
    ec, "boost::filesystem::rename");
}

BOOST_FILESYSTEM_DECL
void resize_file(const path& p, uintmax_t size, system::error_code* ec)
{
# if defined(BOOST_POSIX_API)
  if (BOOST_UNLIKELY(size > static_cast< uintmax_t >((std::numeric_limits< off_t >::max)()))) {
    error(system::errc::file_too_large, p, ec, "boost::filesystem::resize_file");
    return;
  }
# endif
  error(!BOOST_RESIZE_FILE(p.c_str(), size) ? BOOST_ERRNO : 0, p, ec,
    "boost::filesystem::resize_file");
}

BOOST_FILESYSTEM_DECL
space_info space(const path& p, error_code* ec)
{
# ifdef BOOST_POSIX_API
  struct BOOST_STATVFS vfs;
  space_info info;
  if (!error(::BOOST_STATVFS(p.c_str(), &vfs) ? BOOST_ERRNO : 0,
    p, ec, "boost::filesystem::space"))
  {
    info.capacity
      = static_cast<boost::uintmax_t>(vfs.f_blocks)* BOOST_STATVFS_F_FRSIZE;
    info.free
      = static_cast<boost::uintmax_t>(vfs.f_bfree)* BOOST_STATVFS_F_FRSIZE;
    info.available
      = static_cast<boost::uintmax_t>(vfs.f_bavail)* BOOST_STATVFS_F_FRSIZE;
  }

# else

  ULARGE_INTEGER avail, total, free;
  space_info info;

  if (!error(::GetDiskFreeSpaceExW(p.c_str(), &avail, &total, &free)== 0,
     p, ec, "boost::filesystem::space"))
  {
    info.capacity
      = (static_cast<boost::uintmax_t>(total.HighPart)<< 32)
        + total.LowPart;
    info.free
      = (static_cast<boost::uintmax_t>(free.HighPart)<< 32)
        + free.LowPart;
    info.available
      = (static_cast<boost::uintmax_t>(avail.HighPart)<< 32)
        + avail.LowPart;
  }

# endif

  else
  {
    info.capacity = info.free = info.available = 0;
  }
  return info;
}

BOOST_FILESYSTEM_DECL
file_status status(const path& p, error_code* ec)
{
# ifdef BOOST_POSIX_API

  struct ::stat path_stat;
  if (::stat(p.c_str(), &path_stat)!= 0)
  {
    const int err = errno;
    if (ec != 0)                            // always report errno, even though some
      ec->assign(err, system_category());   // errno values are not status_errors

    if (not_found_error(err))
    {
      return fs::file_status(fs::file_not_found, fs::no_perms);
    }
    if (ec == 0)
      BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::status",
        p, error_code(err, system_category())));
    return fs::file_status(fs::status_error);
  }
  if (ec != 0)
    ec->clear();
  if (S_ISDIR(path_stat.st_mode))
    return fs::file_status(fs::directory_file,
      static_cast<perms>(path_stat.st_mode) & fs::perms_mask);
  if (S_ISREG(path_stat.st_mode))
    return fs::file_status(fs::regular_file,
      static_cast<perms>(path_stat.st_mode) & fs::perms_mask);
  if (S_ISBLK(path_stat.st_mode))
    return fs::file_status(fs::block_file,
      static_cast<perms>(path_stat.st_mode) & fs::perms_mask);
  if (S_ISCHR(path_stat.st_mode))
    return fs::file_status(fs::character_file,
      static_cast<perms>(path_stat.st_mode) & fs::perms_mask);
  if (S_ISFIFO(path_stat.st_mode))
    return fs::file_status(fs::fifo_file,
      static_cast<perms>(path_stat.st_mode) & fs::perms_mask);
  if (S_ISSOCK(path_stat.st_mode))
    return fs::file_status(fs::socket_file,
      static_cast<perms>(path_stat.st_mode) & fs::perms_mask);
  return fs::file_status(fs::type_unknown);

# else  // Windows

  DWORD attr(::GetFileAttributesW(p.c_str()));
  if (attr == 0xFFFFFFFF)
  {
    return process_status_failure(p, ec);
  }

  perms permissions = make_permissions(p, attr);

  //  reparse point handling;
  //    since GetFileAttributesW does not resolve symlinks, try to open a file
  //    handle to discover if the file exists
  if (attr & FILE_ATTRIBUTE_REPARSE_POINT)
  {
    handle_wrapper h(
      create_file_handle(
          p.c_str(),
          0,  // dwDesiredAccess; attributes only
          FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
          0,  // lpSecurityAttributes
          OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS,
          0)); // hTemplateFile
    if (h.handle == INVALID_HANDLE_VALUE)
    {
      return process_status_failure(p, ec);
    }

    if (!is_reparse_point_a_symlink(p))
      return file_status(reparse_file, permissions);
  }

  if (ec != 0) ec->clear();
  return (attr & FILE_ATTRIBUTE_DIRECTORY)
    ? file_status(directory_file, permissions)
    : file_status(regular_file, permissions);

# endif
}

BOOST_FILESYSTEM_DECL
file_status symlink_status(const path& p, error_code* ec)
{
# ifdef BOOST_POSIX_API

  struct ::stat path_stat;
  if (::lstat(p.c_str(), &path_stat)!= 0)
  {
    const int err = errno;
    if (ec != 0)                            // always report errno, even though some
      ec->assign(err, system_category());   // errno values are not status_errors

    if (not_found_error(err)) // these are not errors
    {
      return fs::file_status(fs::file_not_found, fs::no_perms);
    }
    if (ec == 0)
      BOOST_FILESYSTEM_THROW(filesystem_error("boost::filesystem::status",
        p, error_code(err, system_category())));
    return fs::file_status(fs::status_error);
  }
  if (ec != 0)
    ec->clear();
  if (S_ISREG(path_stat.st_mode))
    return fs::file_status(fs::regular_file,
      static_cast<perms>(path_stat.st_mode) & fs::perms_mask);
  if (S_ISDIR(path_stat.st_mode))
    return fs::file_status(fs::directory_file,
      static_cast<perms>(path_stat.st_mode) & fs::perms_mask);
  if (S_ISLNK(path_stat.st_mode))
    return fs::file_status(fs::symlink_file,
      static_cast<perms>(path_stat.st_mode) & fs::perms_mask);
  if (S_ISBLK(path_stat.st_mode))
    return fs::file_status(fs::block_file,
      static_cast<perms>(path_stat.st_mode) & fs::perms_mask);
  if (S_ISCHR(path_stat.st_mode))
    return fs::file_status(fs::character_file,
      static_cast<perms>(path_stat.st_mode) & fs::perms_mask);
  if (S_ISFIFO(path_stat.st_mode))
    return fs::file_status(fs::fifo_file,
      static_cast<perms>(path_stat.st_mode) & fs::perms_mask);
  if (S_ISSOCK(path_stat.st_mode))
    return fs::file_status(fs::socket_file,
      static_cast<perms>(path_stat.st_mode) & fs::perms_mask);
  return fs::file_status(fs::type_unknown);

# else  // Windows

  DWORD attr(::GetFileAttributesW(p.c_str()));
  if (attr == 0xFFFFFFFF)
  {
    return process_status_failure(p, ec);
  }

  if (ec != 0) ec->clear();

  perms permissions = make_permissions(p, attr);

  if (attr & FILE_ATTRIBUTE_REPARSE_POINT)
    return is_reparse_point_a_symlink(p)
           ? file_status(symlink_file, permissions)
           : file_status(reparse_file, permissions);

  return (attr & FILE_ATTRIBUTE_DIRECTORY)
    ? file_status(directory_file, permissions)
    : file_status(regular_file, permissions);

# endif
}

 // contributed by Jeff Flinn
BOOST_FILESYSTEM_DECL
path temp_directory_path(system::error_code* ec)
{
# ifdef BOOST_POSIX_API
  const char* val = 0;

  (val = std::getenv("TMPDIR" )) ||
  (val = std::getenv("TMP"    )) ||
  (val = std::getenv("TEMP"   )) ||
  (val = std::getenv("TEMPDIR"));

#   ifdef __ANDROID__
  const char* default_tmp = "/data/local/tmp";
#   else
  const char* default_tmp = "/tmp";
#   endif
  path p((val != NULL) ? val : default_tmp);

  if (p.empty() || (ec && !is_directory(p, *ec)) || (!ec && !is_directory(p)))
  {
    error(ENOTDIR, p, ec, "boost::filesystem::temp_directory_path");
    return p;
  }

  return p;

# else  // Windows

  const wchar_t* tmp_env = L"TMP";
  const wchar_t* temp_env = L"TEMP";
  const wchar_t* localappdata_env = L"LOCALAPPDATA";
  const wchar_t* userprofile_env = L"USERPROFILE";
  const wchar_t* env_list[] = { tmp_env, temp_env, localappdata_env, userprofile_env };

  path p;
  for (unsigned int i = 0; i < sizeof(env_list) / sizeof(*env_list); ++i)
  {
    std::wstring env = wgetenv(env_list[i]);
    if (!env.empty())
    {
      p = env;
      if (i >= 2)
        p /= L"Temp";
      error_code lcl_ec;
      if (exists(p, lcl_ec) && !lcl_ec && is_directory(p, lcl_ec) && !lcl_ec)
        break;
      p.clear();
    }
  }

  if (p.empty())
  {
    // use a separate buffer since in C++03 a string is not required to be contiguous
    const UINT size = ::GetWindowsDirectoryW(NULL, 0);
    if (BOOST_UNLIKELY(size == 0))
    {
    getwindir_error:
      int errval = ::GetLastError();
      error(errval, ec, "boost::filesystem::temp_directory_path");
      return path();
    }

    boost::scoped_array<wchar_t> buf(new wchar_t[size]);
    if (BOOST_UNLIKELY(::GetWindowsDirectoryW(buf.get(), size) == 0))
      goto getwindir_error;

    p = buf.get();  // do not depend on initial buf size, see ticket #10388
    p /= L"Temp";
  }

  return p;

# endif
}

BOOST_FILESYSTEM_DECL
path system_complete(const path& p, system::error_code* ec)
{
# ifdef BOOST_POSIX_API
  return (p.empty() || p.is_absolute())
    ? p : current_path() / p;

# else
  if (p.empty())
  {
    if (ec != 0) ec->clear();
    return p;
  }
  wchar_t buf[buf_size];
  wchar_t* pfn;
  std::size_t len = get_full_path_name(p, buf_size, buf, &pfn);

  if (error(len == 0 ? BOOST_ERRNO : 0, p, ec, "boost::filesystem::system_complete"))
    return path();

  if (len < buf_size)// len does not include null termination character
    return path(&buf[0]);

  boost::scoped_array<wchar_t> big_buf(new wchar_t[len]);

  return error(get_full_path_name(p, len , big_buf.get(), &pfn)== 0 ? BOOST_ERRNO : 0,
    p, ec, "boost::filesystem::system_complete")
    ? path()
    : path(big_buf.get());
# endif
}

BOOST_FILESYSTEM_DECL
path weakly_canonical(const path& p, system::error_code* ec)
{
  path head(p);
  path tail;
  system::error_code tmp_ec;
  path::iterator itr = p.end();

  for (; !head.empty(); --itr)
  {
    file_status head_status = status(head, tmp_ec);
    if (error(head_status.type() == fs::status_error,
      head, ec, "boost::filesystem::weakly_canonical"))
      return path();
    if (head_status.type() != fs::file_not_found)
      break;
    head.remove_filename();
  }

  bool tail_has_dots = false;
  for (; itr != p.end(); ++itr)
  {
    tail /= *itr;
    // for a later optimization, track if any dot or dot-dot elements are present
    if (itr->native().size() <= 2
      && itr->native()[0] == dot
      && (itr->native().size() == 1 || itr->native()[1] == dot))
      tail_has_dots = true;
  }

  if (head.empty())
    return p.lexically_normal();
  head = canonical(head, tmp_ec);
  if (error(tmp_ec.value(), head, ec, "boost::filesystem::weakly_canonical"))
    return path();
  return tail.empty()
    ? head
    : (tail_has_dots  // optimization: only normalize if tail had dot or dot-dot element
        ? (head/tail).lexically_normal()
        : head/tail);
}

} // namespace detail
} // namespace filesystem
} // namespace boost
