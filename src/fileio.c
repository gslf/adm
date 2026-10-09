#ifdef _WIN32
  #define _CRT_SECURE_NO_WARNINGS
  #include <windows.h>
  #include <io.h>
  #include <fcntl.h>
  #include <stdint.h>
#endif

#include "fileio.h"
#include "buffer.h"
#ifndef _WIN32
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>


static int replace(const char *tmp, const char *dst){

  #ifdef _WIN32
    // rename() on Windows refuses to overwrite an existing file, so the
    // replace has to go through MoveFileEx and its explicit flag.
    if (MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING))
      return 0;
    return -1;

  #else
    return rename(tmp, dst);

  #endif
}

#ifdef _WIN32
static void windows_error(void) {
  DWORD error = GetLastError();
  errno = error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS ? EEXIST :
          error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ENOENT : EACCES;
}
#endif

FILE *file_create(const char *path) {
#ifdef _WIN32
  // Some Windows C runtimes do not implement fopen's C11 "x" mode.
  HANDLE handle = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, NULL);
  if (handle == INVALID_HANDLE_VALUE) {
    windows_error();
    return NULL;
  }
  int fd = _open_osfhandle((intptr_t)handle, _O_WRONLY | _O_BINARY);
  if (fd < 0) {
    CloseHandle(handle);
    remove(path);
    return NULL;
  }
  FILE *stream = _fdopen(fd, "wb");
  if (!stream) {
    _close(fd);
    remove(path);
  }
  return stream;
#else
  return fopen(path, "wbx");
#endif
}

char *file_read(const char *path){
  FILE *fp = fopen(path, "rb");

  if (!fp) return NULL;

  // File lenght mesurement
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return NULL;
  }
  long size = ftell(fp);
  if (size < 0){
    fclose(fp);
    return NULL;
  }
  rewind(fp);

  // Allocate the buffer
  char *buf = malloc(size + 1);
  if (!buf){
    fclose(fp);
    return NULL;
  }

  // Read the file
  size_t n = fread(buf, 1, (size_t)size, fp);
  if (n != (size_t)size || ferror(fp) || memchr(buf, '\0', n)) {
    free(buf);
    fclose(fp);
    errno = EILSEQ;
    return NULL;
  }
  buf[n] = '\0';
  
  fclose(fp);
  return buf;
}


// Stream to a separate file; a failed save leaves the original and dirty state intact.
static int write_atomic(const char *path, const char *data, const buffer *b) {
  size_t length = strlen(path);
  char *tmp = malloc(length + 16);
  if (!tmp)
    return -1;
  FILE *fp = NULL;
#ifdef _WIN32
  snprintf(tmp, length + 16, "%s.swp", path);
  // Exclusive creation avoids overwriting somebody else's swap file.
  fp = file_create(tmp);
#else
  snprintf(tmp, length + 16, "%s.swp.XXXXXX", path);
  int fd = mkstemp(tmp);
  if (fd >= 0) {
    struct stat info;
    mode_t mode;
    if (stat(path, &info) == 0)
      mode = info.st_mode & 0777;
    else {
      mode_t mask = umask(0);
      umask(mask);
      mode = 0666 & ~mask;
    }
    if (fchmod(fd, mode) < 0) {
      close(fd);
      remove(tmp);
      free(tmp);
      return -1;
    }
    fp = fdopen(fd, "wb");
    if (!fp) {
      close(fd);
      remove(tmp);
    }
  }
#endif
  if (!fp) {
    free(tmp);
    return -1;
  }
  int result = 0;
  if (b) {
    for (block *k = b->head; k && result == 0; k = k->next)
      for (int i = 0; i < k->count; i++) {
        size_t n = strlen(k->lines[i]);
        if (fwrite(k->lines[i], 1, n, fp) != n || fputc('\n', fp) == EOF) {
          result = -1;
          break;
        }
      }
  } else {
    size_t n = strlen(data);
    if (fwrite(data, 1, n, fp) != n)
      result = -1;
  }
  if (fflush(fp) != 0)
    result = -1;
#ifndef _WIN32
  if (result == 0 && fsync(fileno(fp)) != 0)
    result = -1;
#endif
  if (fclose(fp) != 0)
    result = -1;
  if (result == 0)
    result = replace(tmp, path);
  if (result != 0)
    remove(tmp);
  free(tmp);
  return result;
}

int file_write(const char *path, const char *data) {
  return write_atomic(path, data, NULL);
}

int file_write_buffer(const char *path, const buffer *b) {
  return write_atomic(path, NULL, b);
}

int file_rename(const char *source, const char *destination) {
#ifdef _WIN32
  DWORD attributes = GetFileAttributesA(source);
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    windows_error();
    return -1;
  }
  if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
    errno = EISDIR;
    return -1;
  }
#else
  struct stat info;
  if (lstat(source, &info) < 0)
    return -1;
  if (S_ISDIR(info.st_mode)) {
    errno = EISDIR;
    return -1;
  }
#endif
  if (!strcmp(source, destination))
    return 0;
#ifdef _WIN32
  if (MoveFileExA(source, destination, 0))
    return 0;
  windows_error();
  return -1;
#else
#if defined(__linux__) && defined(SYS_renameat2)
  // RENAME_NOREPLACE (1) is atomic and works on filesystems without hard links.
  if (syscall(SYS_renameat2, AT_FDCWD, source, AT_FDCWD, destination, 1) == 0)
    return 0;
  if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP)
    return -1;
#endif
  // Both names are in the same directory. Exclusive linking prevents a race
  // from overwriting another file, unlike a stat() check followed by rename().
  if (link(source, destination) < 0)
    return -1;
  if (unlink(source) == 0)
    return 0;
  int error = errno;
  unlink(destination);
  errno = error;
  return -1;
#endif
}
