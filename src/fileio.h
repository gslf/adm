#ifndef FILEIO_H
#define FILEIO_H

char *file_read(const char *path);
struct buffer;
int file_write_buffer(const char *path, const struct buffer *b);
int file_write(const char *path, const char *data);

#endif
