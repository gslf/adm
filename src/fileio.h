#ifndef FILEIO_H
#define FILEIO_H

#include <stdio.h>

char *file_read(const char *path);
struct buffer;
int file_write_buffer(const char *path, const struct buffer *b);
int file_write(const char *path, const char *data);
// Rename a file without replacing an existing destination.
int file_rename(const char *source, const char *destination);
FILE *file_create(const char *path);

#endif
