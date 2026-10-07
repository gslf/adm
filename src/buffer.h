#ifndef BUFFER_H
#define BUFFER_H

#include <stddef.h>

#define BLOCK_LINES 1024

typedef struct block {
  char *lines[BLOCK_LINES];
  size_t chars[BLOCK_LINES], char_total;
  int count, chars_valid;
  struct block *next;
} block;

// Replace [row:col, old_row:old_col) with text ending at new_row:new_col.
typedef struct buffer_edit {
  int row, col;
  int old_row, old_col;
  int new_row, new_col;
} buffer_edit;

typedef struct buffer_patch buffer_patch;
#ifdef ADM_TEST_ALLOC
void buffer_test_fail_after(long count);
#endif

typedef struct buffer {
  block *head;
  block *tail;
  int nlines;
  size_t char_total;
  int chars_valid;
  block *lookup;
  int lookup_row;
  void (*on_edit)(struct buffer *b, const buffer_edit *edit, void *context);
  void *edit_context;
  // Vetoable change observer: called after allocation, before text is mutated.
  int (*before_change)(struct buffer *b, const buffer_edit *edit,
                        const char *inserted, size_t bytes, int before_lines,
                        int after_lines, void *context);
  void *change_context;
  int external_edits; // Whole-line replacement supplies precise view events later.
} buffer;

void buffer_init(buffer *b);
int load(buffer *b, const char *filename);
int buffer_load_text(buffer *b, const char *text);
char *buffer_line(const buffer *b, int index);
size_t buffer_char_count(buffer *b); // characters, newlines included
int buffer_insert_char(buffer *b, int row, int col, char c);
int buffer_insert_newline(buffer *b, int row, int col);
int buffer_delete_char(buffer *b, int row, int col);
void buffer_remove_line(buffer *b, int index);
int buffer_join_line(buffer *b, int row);
// Takes ownership on success. Caller emits its precise edit events.
int buffer_replace_line(buffer *b, int row, char *owned);
int buffer_replace_range(buffer *b, int row, int col, int length, const char *text);
int buffer_replace_span(buffer *b, int row, int col, int er, int ec,
                        const char *text, size_t bytes, buffer_edit *result);
// Prepared splices can be reverted without allocation during atomic undo.
buffer_patch *buffer_patch_prepare(buffer *b, int row, int col, int er, int ec,
                                   const char *text, size_t bytes, int empty);
void buffer_patch_apply(buffer_patch *p);
void buffer_patch_revert(buffer_patch *p);
const buffer_edit *buffer_patch_edit(const buffer_patch *p);
void buffer_patch_dispose(buffer_patch *p, int committed);
void buffer_free(buffer *b);

#endif
