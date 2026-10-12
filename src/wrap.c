#include "wrap.h"
#include "utf8.h"

static int blank(char c) {
  return c == ' ' || c == '\t';
}

// End of the cluster at i, with its width in *width. Plain ASCII followed by
// ASCII is a whole cluster of one column, which skips decoding long lines.
static int cluster(const char *s, int i, int *width) {
  if ((unsigned char)s[i] < 0x80 && (unsigned char)s[i + 1] < 0x80) {
    *width = 1;
    return i + 1;
  }
  *width = grapheme_width(s, i);
  return grapheme_next(s, i);
}

int wrap_next(const char *line, int start, int width) {
  if (width < 1)
    width = 1;
  // Text stops one column early; only a blank or the line end takes the last.
  int limit = width > 1 ? width - 1 : 1;
  int used = 0, word = 0, after_blank = -1;
  for (int j = start; ; ) {
    if (!line[j])
      return used < width ? -1 : j;
    int w, next = cluster(line, j, &w);
    int space = blank(line[j]);
    if (used > 0 && used + w > limit && !(space && used + w <= width))
      return after_blank >= 0 ? after_blank : j;
    used += w;
    // Leading indentation is no place to break: the row would hold nothing.
    if (space && word)
      after_blank = next;
    else if (!space)
      word = 1;
    if (next <= j)
      return -1;
    j = next;
  }
}

int wrap_rows(const char *line, int width) {
  int rows = 1;
  for (int start = 0; (start = wrap_next(line, start, width)) >= 0; )
    rows++;
  return rows;
}

int wrap_row(const char *line, int width, int at, int *start) {
  int row = 0, first = 0, next;
  while ((next = wrap_next(line, first, width)) >= 0 && next <= at) {
    first = next;
    row++;
  }
  if (start)
    *start = first;
  return row;
}

int wrap_start(const char *line, int width, int row) {
  int first = 0, next;
  while (row-- > 0 && (next = wrap_next(line, first, width)) >= 0)
    first = next;
  return first;
}

int wrap_col(const char *line, int start, int at) {
  int col = 0;
  for (int j = start; j < at && line[j]; ) {
    int w, next = cluster(line, j, &w);
    if (next <= j)
      break;
    col += w;
    j = next;
  }
  return col;
}

int wrap_byte(const char *line, int start, int width, int col) {
  int stop = wrap_next(line, start, width), c = 0, j = start;
  while (line[j]) {
    int w, next = cluster(line, j, &w);
    if (c + w > col)
      return j;
    // The next row owns its first byte, so this row ends on its last cluster.
    if (next <= j || (stop >= 0 && next >= stop))
      return j;
    c += w;
    j = next;
  }
  return j;
}
