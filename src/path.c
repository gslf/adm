#include "path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

char *path_join(const char *directory, const char *name) {
  size_t length = strlen(directory), extra = strlen(name);
  char *path = malloc(length + extra + 2);
  if (path)
    snprintf(path, length + extra + 2, "%s%s%s", directory,
             length && directory[length - 1] == '/' ? "" : "/", name);
  return path;
}

char *path_absolute(const char *path) {
#ifdef _WIN32
  return _fullpath(NULL, path, 0);
#else
  return realpath(path, NULL);
#endif
}

char *path_current_directory(void) {
#ifdef _WIN32
  return _getcwd(NULL, 0);
#else
  return getcwd(NULL, 0);
#endif
}

const char *path_name(const char *path) {
  const char *name = path;
  for (const char *p = path; *p; p++)
    if (*p == '/'
#ifdef _WIN32
        || *p == '\\'
#endif
       )
      name = p + 1;
  return *name ? name : path;
}
