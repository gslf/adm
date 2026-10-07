#include "buffer.h"
#include "fileio.h"
#include "utf8.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

static block *append_block(buffer *b) {
  block *blk = malloc(sizeof(block));
  if (!blk)
    return NULL;

  blk->count = 0;
  blk->chars_valid = 0;
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

  if (len > INT_MAX - 1 || b->nlines == INT_MAX)
    return -1;
  char *line = malloc(len + 1);
  if (!line)
    return -1;

  memcpy(line, start, len);
  line[len] = '\0';

  b->tail->chars[b->tail->count] = SIZE_MAX;
  b->tail->chars_valid = b->chars_valid = 0;
  b->tail->lines[b->tail->count++] = line;
  b->nlines++;
  return 0;
}

void buffer_init(buffer *b) {
  *b = (buffer){0};
}

// Range replacements let observers track positions without knowing storage.
static void notify_edit(buffer *b, buffer_edit edit) {
  b->chars_valid = 0;
  if (b->on_edit)
    b->on_edit(b, &edit, b->edit_context);
}

static int before_edit(buffer *b, buffer_edit edit, const char *text, size_t bytes,
                       int before, int after) {
  return !b->before_change || b->before_change(b, &edit, text, bytes,
                                               before, after, b->change_context);
}

// Stream into line blocks: opening a huge file never allocates a second file-sized copy.
int load(buffer *b, const char *filename) {
  buffer_init(b);
  if (!filename)
    return 0;
  struct stat info;
  if (stat(filename, &info) < 0)
    return errno == ENOENT ? 0 : -1;
#ifdef _WIN32
  if ((info.st_mode & _S_IFMT) != _S_IFREG)
#else
  if (!S_ISREG(info.st_mode))
#endif
    return -1;
  FILE *fp = fopen(filename, "rb");
  if (!fp)
    return errno == ENOENT ? 0 : -1;
  char chunk[65536], *line = NULL;
  size_t length = 0, capacity = 0, n;
  int first = 1, result = 0;
  while (result == 0 && (n = fread(chunk, 1, sizeof chunk, fp)) > 0) {
    for (size_t i = 0; i < n; i++) {
      unsigned char c = (unsigned char)chunk[i];
      if (!c) {
        errno = EILSEQ;
        result = -1;
        break;
      }
      if (c != '\n') {
        if (length >= INT_MAX - 1) {
          result = -1;
          break;
        }
        if (length == capacity) {
          size_t next = capacity ? capacity * 2 : 256;
          char *grown = realloc(line, next);
          if (!grown) {
            result = -1;
            break;
          }
          line = grown;
          capacity = next;
        }
        line[length++] = (char)c;
        continue;
      }
      size_t offset = first && length >= 3 &&
          !memcmp(line, "\xef\xbb\xbf", 3) ? 3 : 0;
      if (length > offset && line[length - 1] == '\r')
        length--;
      if (push_line(b, line ? line + offset : "", length - offset) < 0) {
        result = -1;
        break;
      }
      first = 0;
      length = 0;
    }
  }
  if (ferror(fp))
    result = -1;
  if (result == 0 && length) {
    size_t offset = first && length >= 3 &&
        !memcmp(line, "\xef\xbb\xbf", 3) ? 3 : 0;
    // A BOM alone is an empty file, matching buffer_load_text.
    if (length > offset) {
      if (line[length - 1] == '\r')
        length--;
      if (push_line(b, line + offset, length - offset) < 0)
        result = -1;
    }
  }
  free(line);
  if (fclose(fp) != 0)
    result = -1;
  if (result < 0)
    buffer_free(b);
  return result;
}

int buffer_load_text(buffer *b, const char *text) {
  buffer_init(b);
  // Drop a UTF-8 byte order mark, it is not part of the text.
  const char *start = text;
  if ((unsigned char)start[0] == 0xEF && (unsigned char)start[1] == 0xBB &&
      (unsigned char)start[2] == 0xBF)
    start += 3;

  const char *line = start;
  for (const char *p = start; *p; p++) {
    if (*p == '\n') {
      size_t len = (size_t)(p - line);

      // Manage WINDOWS CRLF
      if (len > 0 && line[len - 1] == '\r')
        len--;

      // Push a new line
      if (push_line(b, line, len) == -1) {
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
      buffer_free(b);
      return -1;
    }
  }

  return 0;
}

size_t buffer_char_count(buffer *b) {
  if (b->chars_valid)
    return b->char_total;
  size_t total = b->nlines > 1 ? (size_t)b->nlines - 1 : 0;
  for (block *k = b->head; k; k = k->next) {
    if (!k->chars_valid) {
      k->char_total = 0;
      for (int i = 0; i < k->count; i++) {
        if (k->chars[i] == SIZE_MAX) {
          size_t count = 0;
          const char *s = k->lines[i];
          for (int j = 0; s[j]; j = grapheme_next(s, j))
            count++;
          k->chars[i] = count;
        }
        k->char_total += k->chars[i];
      }
      k->chars_valid = 1;
    }
    total += k->char_total;
  }
  b->chars_valid = 1;
  return b->char_total = total;
}

static block *locate(buffer *b, int *index) {
  if (*index < 0 || *index >= b->nlines)
    return NULL;
  int start = 0;
  block *blk = b->head;
  if (b->lookup && *index >= b->lookup_row) {
    blk = b->lookup;
    start = b->lookup_row;
  }
  while (blk && *index - start >= blk->count) {
    start += blk->count;
    blk = blk->next;
  }
  b->lookup = blk;
  b->lookup_row = start;
  *index -= start;
  return blk;
}

char *buffer_line(const buffer *b, int index) {
  block *blk = locate((buffer *)b, &index);
  return blk ? blk->lines[index] : NULL;
}

// Only touched lines need recounting; idle rendering has an O(1) character count.
static void invalidate_line(buffer *b, int row) {
  block *blk = locate(b, &row);
  if (blk) {
    blk->chars[row] = SIZE_MAX;
    blk->chars_valid = b->chars_valid = 0;
  }
}

// Return the address of the char* slot holding line `index`, or NULL.
static char **line_slot(buffer *b, int index) {
  block *blk = locate(b, &index);
  return blk ? &blk->lines[index] : NULL;
}

int buffer_insert_char(buffer *b, int row, int col, char c) {
  int before = b->nlines;
  if (!before) {
    char text[2] = {c, 0};
    return c ? buffer_replace_span(b, 0, 0, 0, 0, text, 1, NULL) : -1;
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

  if (len > INT_MAX - 2)
    return -1;
  char *nl = realloc(line, (size_t)len + 2);
  if (!nl)
    return -1;

  *slot = nl;
  if (!before_edit(b, (buffer_edit){row, col, row, col, row, col + 1},
                   &c, 1, before, b->nlines)) {
    return -1;
  }
  memmove(nl + col + 1, nl + col, len - col + 1); // shift, terminator included
  nl[col] = c;
  *slot = nl;
  invalidate_line(b, row);
  notify_edit(b, (buffer_edit){row, col, row, col, row, col + 1});
  return 0;
}

int buffer_insert_newline(buffer *b, int row, int col) {
  const char *line = buffer_line(b, row);
  if (!b->nlines) row = col = 0;
  else {
    if (!line) return -1;
    int size = (int)strlen(line);
    if (col < 0) col = 0;
    if (col > size) col = size;
  }
  return buffer_replace_span(b, row, col, row, col, "\n", 1, NULL);
}

void buffer_remove_line(buffer *b, int index) {
  if (index < 0 || index >= b->nlines) return;
  int row = index, col = 0, er = index + 1, ec = 0;
  if (er == b->nlines) {
    er = index; ec = (int)strlen(buffer_line(b, index));
    if (index) { row--; col = (int)strlen(buffer_line(b, row)); }
  }
  buffer_patch *p = buffer_patch_prepare(b, row, col, er, ec, "", 0, b->nlines == 1);
  if (!p) return;
  buffer_edit edit = *buffer_patch_edit(p);
  if (!before_edit(b, edit, "", 0, b->nlines, b->nlines - 1)) {
    buffer_patch_dispose(p, 0); return;
  }
  buffer_patch_apply(p);
  notify_edit(b, edit);
  buffer_patch_dispose(p, 1);
}

int buffer_delete_char(buffer *b, int row, int col) {
  char **slot = line_slot(b, row);
  if (!slot)
    return -1;

  char *line = *slot;
  int len = (int)strlen(line);
  if (col < 0 || col >= len)
    return -1; // nothing to delete here

  if (!before_edit(b, (buffer_edit){row, col, row, col + 1, row, col},
                   "", 0, b->nlines, b->nlines)) return -1;
  memmove(line + col, line + col + 1, len - col); // tail, terminator included
  char *nl = realloc(line, len);                  // len-1 chars + '\0'
  if (nl)
    *slot = nl; // if realloc fails, line is still valid
  invalidate_line(b, row);
  notify_edit(b, (buffer_edit){row, col, row, col + 1, row, col});
  return 0;
}

int buffer_join_line(buffer *b, int row) {
  const char *line = buffer_line(b, row);
  if (!line || row + 1 >= b->nlines) return -1;
  int col = (int)strlen(line);
  return buffer_replace_span(b, row, col, row + 1, 0, "", 0, NULL);
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

// One allocation and one rebase event per replacement, even for huge lines.
int buffer_replace_range(buffer *b, int row, int col, int length, const char *text) {
  size_t add = strlen(text);
  if (!b->nlines) {
    if (row || col || length) return -1;
    return buffer_replace_span(b, 0, 0, 0, 0, text, add, NULL);
  }
  char **slot = line_slot(b, row);
  if (!slot || col < 0 || length < 0 || strchr(text, '\n')) return -1;
  size_t old = strlen(*slot);
  if ((size_t)col > old || (size_t)length > old - col ||
      add > (size_t)INT_MAX - 1 - (old - length)) return -1;
  if (!length && !add) return 0;
  size_t size = old - length + add;
  char *next = malloc(size + 1);
  if (!next) return -1;
  memcpy(next, *slot, (size_t)col);
  memcpy(next + col, text, add);
  memcpy(next + col + add, *slot + col + length, old - col - length + 1);
  buffer_edit edit = {row, col, row, col + length, row, col + (int)add};
  if (!before_edit(b, edit, text, add, b->nlines, b->nlines)) { free(next); return -1; }
  free(*slot); *slot = next;
  invalidate_line(b, row);
  notify_edit(b, edit);
  return 0;
}

int buffer_replace_line(buffer *b, int row, char *owned) {
  char **slot = line_slot(b, row);
  if (!slot || !owned) return -1;
  size_t old = strlen(*slot), size = strlen(owned), start = 0, suffix = 0;
  if (size > INT_MAX - 1) return -1;
  while (start < old && start < size && (*slot)[start] == owned[start]) start++;
  while (suffix < old - start && suffix < size - start &&
         (*slot)[old - suffix - 1] == owned[size - suffix - 1]) suffix++;
  buffer_edit edit = {row, (int)start, row, (int)(old - suffix), row, (int)(size - suffix)};
  b->external_edits = 1;
  int ok = (start == old && start == size) ||
      before_edit(b, edit, owned + start, size - start - suffix, b->nlines, b->nlines);
  b->external_edits = 0;
  if (!ok) return -1;
  free(*slot); *slot = owned;
  invalidate_line(b, row);
  return 0;
}
#ifdef ADM_TEST_ALLOC
static long patch_budget = -1;
void buffer_test_fail_after(long count) { patch_budget = count; }
static void *patch_malloc(size_t size) {
  if (patch_budget == 0) return NULL;
  if (patch_budget > 0) patch_budget--;
  return malloc(size);
}
#else
#define patch_malloc malloc
#endif

// A prepared splice owns only touched blocks and changed lines. Untouched line
// pointers are shared until commit. Rollback requires no allocation, permitting
// a multi-operation undo to fail without leaving a partially restored document.
struct buffer_patch {
  buffer *buf;
  block *previous, *old_head, *old_tail, *next, *new_head, *new_tail;
  int old_start, old_end, old_count, new_count;
  int new_start, new_end;
  buffer_edit edit;
  block *line_block;
  int line_index;
  char *line_before, *line_after;
};
static void free_patch_blocks(block *head, block *tail, int from, int to) {
  int row = 0;
  while (head) {
    block *next = head == tail ? NULL : head->next;
    for (int i = 0; i < head->count; i++, row++)
      if (row >= from && row <= to) free(head->lines[i]);
    free(head);
    head = next;
  }
}
static int patch_add(buffer_patch *p, char *line) {
  if (!p->new_tail || p->new_tail->count == BLOCK_LINES) {
    block *b = patch_malloc(sizeof *b);
    if (b) memset(b, 0, sizeof *b);
    if (!b) return -1;
    if (p->new_tail) p->new_tail->next = b;
    else p->new_head = b;
    p->new_tail = b;
  }
  block *b = p->new_tail;
  b->chars[b->count] = SIZE_MAX;
  b->lines[b->count++] = line;
  p->new_count++;
  return 0;
}
buffer_patch *buffer_patch_prepare(buffer *b, int row, int col, int er, int ec,
                                   const char *text, size_t bytes, int empty) {
  int virtual = !b->nlines;
  if (row < 0 || er < row || col < 0 || ec < 0 ||
      (virtual ? row || er || col || ec : er >= b->nlines) ||
      (row == er && ec < col) || memchr(text, 0, bytes)) return NULL;
  const char *first = virtual ? "" : buffer_line(b, row);
  const char *last = virtual ? "" : buffer_line(b, er);
  size_t lastlen = strlen(last);
  if ((size_t)col > strlen(first) || (size_t)ec > lastlen) return NULL;
  buffer_patch *p = patch_malloc(sizeof *p);
  if (p) memset(p, 0, sizeof *p);
  if (!p) return NULL;
  p->buf = b;
  // Single-line replay retains two strings and swaps one slot. Dense replace-all
  // therefore never duplicates a block's pointer table for every changed row.
  if (!virtual && row == er && !empty && !memchr(text, '\n', bytes)) {
    size_t prefix = (size_t)col, suffix = lastlen - (size_t)ec;
    if (bytes > (size_t)INT_MAX - 1 - prefix ||
        suffix > (size_t)INT_MAX - 1 - prefix - bytes) { free(p); return NULL; }
    size_t size = prefix + bytes + suffix;
    char *line = patch_malloc(size + 1);
    if (!line) { free(p); return NULL; }
    memcpy(line, first, prefix);
    memcpy(line + prefix, text, bytes);
    memcpy(line + prefix + bytes, last + ec, suffix);
    line[size] = 0;
    int index = row;
    p->line_block = locate(b, &index);
    p->line_index = index;
    p->line_before = p->line_block->lines[index];
    p->line_after = line;
    p->edit = (buffer_edit){row, col, er, ec, row, col + (int)bytes};
    return p;
  }
  p->old_start = p->old_end = -1;
  p->new_start = 0; p->new_end = -1;
  int start = 0;
  block *previous = NULL, *at = b->head;
  while (at && row >= start + at->count) {
    start += at->count; previous = at; at = at->next;
  }
  p->previous = previous;
  p->old_head = at;
  int local = 0;
  if (at) {
    p->old_start = row - start;
    p->old_end = er - start;
    block *end = at;
    int through = start + at->count;
    while (er >= through) { end = end->next; through += end->count; }
    p->old_tail = end; p->next = end->next;
    p->old_count = through - start;
    for (int i = 0; i < p->old_start; i++)
      if (patch_add(p, at->lines[i]) < 0) goto fail;
    local = p->new_count;
  }
  p->new_start = local;
  size_t begin = 0;
  int lines = 0, endcol = col;
  for (size_t i = 0; i <= bytes; i++) {
    if (i != bytes && text[i] != '\n') continue;
    size_t prefix = lines == 0 ? (size_t)col : 0;
    size_t suffix = i == bytes ? lastlen - (size_t)ec : 0;
    if (i - begin > (size_t)INT_MAX - 1 - prefix ||
        suffix > (size_t)INT_MAX - 1 - prefix - (i - begin)) goto fail;
    size_t size = prefix + i - begin + suffix;
    if (!(empty && !size && !lines && i == bytes)) {
      char *line = patch_malloc(size + 1);
      if (!line) goto fail;
      if (prefix) memcpy(line, first, prefix);
      memcpy(line + prefix, text + begin, i - begin);
      if (suffix) memcpy(line + prefix + i - begin, last + ec, suffix);
      line[size] = 0;
      if (patch_add(p, line) < 0) { free(line); goto fail; }
      p->new_end = p->new_count - 1;
    }
    if (i == bytes) endcol = (int)(prefix + i - begin);
    else {
      if (lines == INT_MAX) goto fail;
      lines++;
    }
    begin = i + 1;
  }
  if (at) {
    int index = 0;
    for (block *k = at; k; k = k == p->old_tail ? NULL : k->next)
      for (int i = 0; i < k->count; i++, index++)
        if (index > p->old_end && patch_add(p, k->lines[i]) < 0) goto fail;
  }
  if (p->new_count > INT_MAX - (b->nlines - p->old_count)) goto fail;
  p->edit = (buffer_edit){row, col, er, ec, row + lines, endcol};
  return p;
fail:
  free_patch_blocks(p->new_head, p->new_tail, p->new_start, p->new_end);
  free(p); return NULL;
}
static void patch_link(buffer_patch *p, int forward) {
  buffer *b = p->buf;
  if (p->line_block) {
    p->line_block->lines[p->line_index] = forward ? p->line_after : p->line_before;
    p->line_block->chars[p->line_index] = SIZE_MAX;
    p->line_block->chars_valid = b->chars_valid = 0;
    return;
  }
  block *head = forward ? p->new_head : p->old_head;
  block *tail = forward ? p->new_tail : p->old_tail;
  if (tail) tail->next = p->next;
  if (p->previous) p->previous->next = head ? head : p->next;
  else b->head = head ? head : p->next;
  if (!p->next) b->tail = tail ? tail : p->previous;
  b->nlines += forward ? p->new_count - p->old_count : p->old_count - p->new_count;
  b->lookup = NULL; b->chars_valid = 0;
}
void buffer_patch_apply(buffer_patch *p) { patch_link(p, 1); }
void buffer_patch_revert(buffer_patch *p) { patch_link(p, 0); }
const buffer_edit *buffer_patch_edit(const buffer_patch *p) { return &p->edit; }
void buffer_patch_dispose(buffer_patch *p, int committed) {
  if (!p) return;
  if (p->line_block) free(committed ? p->line_before : p->line_after);
  else if (committed) free_patch_blocks(p->old_head, p->old_tail, p->old_start, p->old_end);
  else free_patch_blocks(p->new_head, p->new_tail, p->new_start, p->new_end);
  free(p);
}
int buffer_replace_span(buffer *b, int row, int col, int er, int ec,
                        const char *text, size_t bytes, buffer_edit *result) {
  buffer_patch *p = buffer_patch_prepare(b, row, col, er, ec, text, bytes, 0);
  if (!p) return -1;
  buffer_edit edit = *buffer_patch_edit(p);
  int after = b->nlines - p->old_count + p->new_count;
  if (b->before_change && !b->before_change(b, &edit, text, bytes,
                                           b->nlines, after, b->change_context)) {
    buffer_patch_dispose(p, 0); return -1;
  }
  buffer_patch_apply(p);
  notify_edit(b, edit);
  buffer_patch_dispose(p, 1);
  if (result) *result = edit;
  return 0;
}
