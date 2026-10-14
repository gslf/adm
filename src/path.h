#ifndef PATH_H
#define PATH_H

char *path_join(const char *directory, const char *name);
char *path_absolute(const char *path);
char *path_current_directory(void);
int path_is_directory(const char *path);
const char *path_name(const char *path);

#endif
