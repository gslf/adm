#ifndef BUFFER_H
#define BUFFER_H

#define BLOCK_LINES 1024

typedef struct block {
  char *lines[BLOCK_LINES];
  int count;
  struct block *next;
} block;

typedef struct {
  block *head;
  block *tail;
  int nlines;
} buffer;

void buffer_init(buffer *b);
int load(buffer *b, const char *filename);
char *buffer_line(const buffer *b, int index);
int buffer_insert_char(buffer *b, int row, int col, char c);
int buffer_insert_newline(buffer *b, int row, int col);
int buffer_delete_char(buffer *b, int row, int col);
int buffer_join_line(buffer *b, int row);
void buffer_free(buffer *b);

#endif

