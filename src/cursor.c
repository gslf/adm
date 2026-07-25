#include "cursor.h"
#include "screen.h"

#include <string.h>

static int line_len(editor *e, int row) {
  char *l = buffer_line(&e->buf, row);
  return l ? (int)strlen(l) : 0;
}

// Snap cx to the length of the current line (shorter lines).
static void snap(editor *e) {
  int len = line_len(e, e->cy);
  if (e->cx > len)
    e->cx = len;
}

void cursor_up(editor *e) {
  if (e->cy > 0)
    e->cy--;
  snap(e);
}

void cursor_down(editor *e) {
  if (e->cy < e->buf.nlines - 1)
    e->cy++;
  snap(e);
}

void cursor_left(editor *e) {
  if (e->cx > 0) {
    e->cx--;
  } else if (e->cy > 0) {
    // At line start: move to the end of the previous line.
    e->cy--;
    e->cx = line_len(e, e->cy);
  }
}

void cursor_right(editor *e) {
  int len = line_len(e, e->cy);
  if (e->cx < len) {
    e->cx++;
  } else if (e->cy < e->buf.nlines - 1) {
    // At line end: move to the start of the next line.
    e->cy++;
    e->cx = 0;
  }
}

void cursor_scroll(editor *e) {
  int th = e->rows - 2; // two status bars
  if (th < 1)
    th = 1;
  int tw = e->cols - screen_gutter(e);
  if (tw < 1)
    tw = 1;

  // Vertical
  if (e->cy < e->rowoff)
    e->rowoff = e->cy;
  if (e->cy >= e->rowoff + th)
    e->rowoff = e->cy - th + 1;

  // Horizontal
  if (e->cx < e->coloff)
    e->coloff = e->cx;
  if (e->cx >= e->coloff + tw)
    e->coloff = e->cx - tw + 1;
}
