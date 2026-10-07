#include "path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
static char *normalize(char *path) {
  for (char *p = path; p && *p; p++)
    if (*p == '\\')
      *p = '/';
  return path;
}
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
  return normalize(_fullpath(NULL, path, 0));
#else
  char *resolved = realpath(path, NULL);
  if (resolved)
    return resolved;
  // A startup file may not exist until its first save. Resolve its parent.
  const char *name = strrchr(path, '/');
  if (!name) {
    char *cwd = path_current_directory();
    char *result = cwd ? path_join(cwd, path) : NULL;
    free(cwd);
    return result;
  }
  size_t length = (size_t)(name - path);
  char *parent = malloc(length + 2);
  if (!parent)
    return NULL;
  memcpy(parent, path, length);
  parent[length] = '\0';
  resolved = realpath(length ? parent : "/", NULL);
  free(parent);
  char *result = resolved ? path_join(resolved, name + 1) : NULL;
  free(resolved);
  return result;
#endif
}

char *path_current_directory(void) {
#ifdef _WIN32
  return normalize(_getcwd(NULL, 0));
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
