#ifndef BUFFER_H
#define BUFFER_H

#define BLOCK_LINES 1024

typedef struct block {
  char *lines[BLOCK_LINES];
  int count;
  struct block *next;
} block;

// Replace [row:col, old_row:old_col) with text ending at new_row:new_col.
typedef struct buffer_edit {
  int row, col;
  int old_row, old_col;
  int new_row, new_col;
} buffer_edit;

typedef struct buffer {
  block *head;
  block *tail;
  int nlines;
  void (*on_edit)(struct buffer *b, const buffer_edit *edit, void *context);
  void *edit_context;
} buffer;

void buffer_init(buffer *b);
int load(buffer *b, const char *filename);
int buffer_load_text(buffer *b, const char *text);
char *buffer_line(const buffer *b, int index);
int buffer_char_count(const buffer *b); // characters, newlines included
int buffer_insert_char(buffer *b, int row, int col, char c);
int buffer_insert_newline(buffer *b, int row, int col);
int buffer_delete_char(buffer *b, int row, int col);
void buffer_remove_line(buffer *b, int index);
int buffer_join_line(buffer *b, int row);
void buffer_free(buffer *b);

#endif
