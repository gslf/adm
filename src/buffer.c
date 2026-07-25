#include "buffer.h"
#include "fileio.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static block *append_block(buffer *b) {
  block *blk = malloc(sizeof(block));
  if (!blk)
    return NULL;

  blk->count = 0;
  blk->next = NULL;

  if (b->tail)
    b->tail->next = blk;
  else
    b->head = blk;
  b->tail = blk;

  return blk;
}


static int push_line(buffer *b, const char *start, size_t len) {
  if (!b->tail || b->tail->count == BLOCK_LINES) {
    if (!append_block(b))
      return -1;
  }

  char *line = malloc(len + 1);
  if (!line)
    return -1;

  memcpy(line, start, len);
  line[len] = '\0';

  b->tail->lines[b->tail->count++] = line;
  b->nlines++;
  return 0;
}

void buffer_init(buffer *b) {
  b->head = NULL;
  b->tail = NULL;
  b->nlines = 0;
}

int load(buffer *b, const char *filename) {
  buffer_init(b);

  if (!filename)
    return 0;

  errno = 0;
  char *text = file_read(filename);
  if (!text) {
    // IF the file do not exist, use an empty buffer
    if (errno == ENOENT)
      return 0;
    
    return -1;
  }

  const char *line = text;
  for (const char *p = text; *p; p++) {
    if (*p == '\n') {
      size_t len = (size_t)(p - line);

      // Manage WINDOWS CRLF
      if (len > 0 && line[len - 1] == '\r')
        len--;

      // Push a new line
      if (push_line(b, line, len) == -1) {
        free(text);
        buffer_free(b);
        return -1;
      }

      // Move the line start cursor
      line = p + 1;
    }
  }

  // Last line
  if (*line) {
    size_t len = strlen(line);

    // Manage WINDOWS CRLF
    if (len > 0 && line[len - 1] == '\r')
      len--;

    if (push_line(b, line, len) == -1) {
      free(text);
      buffer_free(b);
      return -1;
    }
  }

  free(text);
  return 0;
}

char *buffer_line(const buffer *b, int index) {
  if (index < 0 || index >= b->nlines)
    return NULL;

  // Walk the blocks accumulating their counts.
  block *blk = b->head;
  while (blk && index >= blk->count) {
    index -= blk->count;
    blk = blk->next;
  }

  if (!blk)
    return NULL;

  return blk->lines[index];
}

// Allocate an empty (zero-length) line.
static char *empty_line(void) {
  char *s = malloc(1);
  if (s)
    s[0] = '\0';
  return s;
}

// Return the address of the char* slot holding line `index`, or NULL.
static char **line_slot(buffer *b, int index) {
  if (index < 0 || index >= b->nlines)
    return NULL;

  block *blk = b->head;
  while (blk && index >= blk->count) {
    index -= blk->count;
    blk = blk->next;
  }
  if (!blk)
    return NULL;

  return &blk->lines[index];
}

// Split a full block into two, moving the upper half to a new block
// inserted right after it. Returns the new block, or NULL on failure.
static block *split_block(buffer *b, block *blk) {
  block *nb = malloc(sizeof(block));
  if (!nb)
    return NULL;

  int half = blk->count / 2;
  int moved = blk->count - half;
  memcpy(nb->lines, &blk->lines[half], moved * sizeof(char *));
  nb->count = moved;
  nb->next = blk->next;

  blk->count = half;
  blk->next = nb;
  if (b->tail == blk)
    b->tail = nb;

  return nb;
}

// Insert an already-allocated line pointer at global position `index`
static int insert_line(buffer *b, int index, char *line) {
  block *blk = b->head;
  int off = index;
  while (blk && off > blk->count) {
    off -= blk->count;
    blk = blk->next;
  }

  // The block is the last, append a new block.
  if (!blk) {
    if (!b->tail || b->tail->count == BLOCK_LINES)
      if (!append_block(b))
        return -1;
    b->tail->lines[b->tail->count++] = line;
    b->nlines++;
    return 0;
  }

  // The block is full, split it and then pick the right half for the insert.
  if (blk->count == BLOCK_LINES) {
    block *nb = split_block(b, blk);
    if (!nb)
      return -1;
    if (off > blk->count) {
      off -= blk->count;
      blk = nb;
    }
  }

  // Insert the line
  memmove(&blk->lines[off + 1], &blk->lines[off],
          (blk->count - off) * sizeof(char *));
  blk->lines[off] = line;
  blk->count++;
  b->nlines++;
  return 0;
}

int buffer_insert_char(buffer *b, int row, int col, char c) {
  // Empty buffer: create the first line so there is something to write on.
  if (b->nlines == 0) {
    char *first = empty_line();
    if (!first || insert_line(b, 0, first) != 0) {
      free(first);
      return -1;
    }
    row = 0;
  }

  char **slot = line_slot(b, row);
  if (!slot)
    return -1;

  char *line = *slot;
  int len = (int)strlen(line);
  if (col < 0)
    col = 0;
  if (col > len)
    col = len;

  char *nl = realloc(line, len + 2);
  if (!nl)
    return -1;

  memmove(nl + col + 1, nl + col, len - col + 1); // shift, terminator included
  nl[col] = c;
  *slot = nl;
  return 0;
}

int buffer_insert_newline(buffer *b, int row, int col) {
  if (b->nlines == 0) {
    char *first = empty_line();
    if (!first || insert_line(b, 0, first) != 0) {
      free(first);
      return -1;
    }
    row = 0;
  }

  char **slot = line_slot(b, row);
  if (!slot)
    return -1;

  char *line = *slot;
  int len = (int)strlen(line);
  if (col < 0)
    col = 0;
  if (col > len)
    col = len;

  // The part from `col` onward becomes a new line after `row`.
  int rlen = len - col;
  char *right = malloc(rlen + 1);
  if (!right)
    return -1;
  memcpy(right, line + col, rlen + 1); // terminator included

  if (insert_line(b, row + 1, right) != 0) {
    free(right);
    return -1;
  }

  // Truncate the left part. Re-find the slot: insertion may have split the
  // block, but the index of `row` is unchanged.
  slot = line_slot(b, row);
  char *left = realloc(*slot, col + 1);
  if (left) {
    left[col] = '\0';
    *slot = left;
  } else {
    (*slot)[col] = '\0'; // shrink failed: truncate in place
  }
  return 0;
}

// Remove the line at global position `index`, freeing it. Drops the block
// if it becomes empty.
static void remove_line(buffer *b, int index) {
  block *blk = b->head, *prev = NULL;
  int off = index;
  while (blk && off >= blk->count) {
    off -= blk->count;
    prev = blk;
    blk = blk->next;
  }
  if (!blk)
    return;

  free(blk->lines[off]);
  memmove(&blk->lines[off], &blk->lines[off + 1],
          (blk->count - off - 1) * sizeof(char *));
  blk->count--;
  b->nlines--;

  if (blk->count == 0) {
    if (prev)
      prev->next = blk->next;
    else
      b->head = blk->next;
    if (b->tail == blk)
      b->tail = prev;
    free(blk);
  }
}

int buffer_delete_char(buffer *b, int row, int col) {
  char **slot = line_slot(b, row);
  if (!slot)
    return -1;

  char *line = *slot;
  int len = (int)strlen(line);
  if (col < 0 || col >= len)
    return -1; // nothing to delete here

  memmove(line + col, line + col + 1, len - col); // tail, terminator included
  char *nl = realloc(line, len);                  // len-1 chars + '\0'
  if (nl)
    *slot = nl; // if realloc fails, line is still valid
  return 0;
}

int buffer_join_line(buffer *b, int row) {
  char **slot = line_slot(b, row);
  char **next = line_slot(b, row + 1);
  if (!slot || !next)
    return -1;

  char *a = *slot;
  char *bn = *next;
  int la = (int)strlen(a);
  int lb = (int)strlen(bn);

  char *merged = realloc(a, la + lb + 1);
  if (!merged)
    return -1;
  memcpy(merged + la, bn, lb + 1); // terminator included
  *slot = merged;

  remove_line(b, row + 1); // frees the old next line
  return 0;
}

void buffer_free(buffer *b) {
  block *blk = b->head;
  while (blk) {
    for (int i = 0; i < blk->count; i++)
      free(blk->lines[i]);

    block *next = blk->next;
    free(blk);
    blk = next;
  }

  buffer_init(b);
}
