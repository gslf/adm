#include "screen.h"
#include "cursor.h"
#include "utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define WRITE _write
#else
#include <unistd.h>
#define WRITE write
#endif

void ab_append(abuf *ab, const char *s, int len) {
  char *nb = realloc(ab->b, ab->len + len);
  if (!nb)
    return; // on OOM skip this chunk, no crash
  memcpy(nb + ab->len, s, len);
  ab->b = nb;
  ab->len += len;
}

void ab_free(abuf *ab) {
  free(ab->b);
  ab->b = NULL;
  ab->len = 0;
}

// Bytes of s that fit in `cols` screen columns, cut on a cluster boundary so
// a multi byte character is never split. Reports the columns actually used.
static int fit_cols(const char *s, int cols, int *used) {
  int c = 0, j = 0;
  while (s[j]) {
    int w = grapheme_width(s, j);
    if (c + w > cols)
      break;
    int n = grapheme_next(s, j);
    if (n <= j)
      break;
    c += w;
    j = n;
  }
  *used = c;
  return j;
}

int screen_gutter(const editor *e) {
  int n = e->buf.nlines;
  if (n < 1)
    n = 1;

  int digits = 1;
  while (n >= 10) {
    n /= 10;
    digits++;
  }
  return digits + 1; // one space column after the number
}

void screen_refresh(editor *e) {
  cursor_scroll(e);

  int th = e->rows - 2; // text rows (two status bars)
  if (th < 1)
    th = 1;
  int gutter = screen_gutter(e);
  int tw = e->cols - gutter; // text area width
  if (tw < 1)
    tw = 1;

  abuf ab = {0};
  char tmp[64];

  ab_append(&ab, "\x1b[?25l", 6); // hide the cursor
  ab_append(&ab, "\x1b[H", 3);    // home

  // --- Top status bar (bright yellow, bold logo) ---
  ab_append(&ab, "\x1b[30;103m", 9);            // black text on bright yellow
  ab_append(&ab, "\x1b[1m ][adm\x1b[22m", 15);  // bold logo, then bold off
  int logolen = 6;                              // visible width of " ][adm"

  char top[512];
  snprintf(top, sizeof top, "  %s%s",
           e->filename ? e->filename : "[No Name]",
           e->dirty ? " **" : "");
  int avail = e->cols - logolen; // room left after the logo
  if (avail < 0)
    avail = 0;
  int tcols;
  int tn = fit_cols(top, avail, &tcols);
  ab_append(&ab, top, tn);
  for (int i = logolen + tcols; i < e->cols; i++)
    ab_append(&ab, " ", 1);
  ab_append(&ab, "\x1b[m", 3);
  ab_append(&ab, "\r\n", 2);

  // --- Text area ---
  int sr = 0, sc = 0, er = 0, ec = 0;
  int has_sel = selection_range(e, &sr, &sc, &er, &ec);

  for (int y = 0; y < th; y++) {
    int filerow = e->rowoff + y;
    ab_append(&ab, "\x1b[K", 3); // clear the line

    if (filerow < e->buf.nlines) {
      int ln = snprintf(tmp, sizeof tmp, "%*d ", gutter - 1, filerow + 1);
      ab_append(&ab, tmp, ln);

      char *line = buffer_line(&e->buf, filerow);
      int len = line ? (int)strlen(line) : 0;

      // Selected byte range on this row, end exclusive. Rows in the middle of
      // a multi line selection run one past the end, to mark the newline.
      int from = 0, to = 0;
      if (has_sel && filerow >= sr && filerow <= er) {
        from = (filerow == sr) ? sc : 0;
        to   = (filerow == er) ? ec : len + 1;
      }

      // Walk the line one grapheme cluster at a time, tracking the screen
      // column so that wide and zero width characters land where they should.
      int rev = 0;  // reverse video currently on
      int col = 0;  // column of the cluster about to be drawn
      for (int j = 0; ; ) {
        int inside = (j >= from && j < to);
        int at_end = (j >= len);
        if (at_end && !inside)
          break; // past the text and outside the selection

        int w = at_end ? 1 : grapheme_width(line, j);
        if (col + w > e->coloff + tw)
          break; // would spill past the right edge

        if (col >= e->coloff || col + w > e->coloff) {
          if (inside != rev) {
            ab_append(&ab, inside ? "\x1b[7m" : "\x1b[27m", inside ? 4 : 5);
            rev = inside;
          }
          if (at_end || col < e->coloff)
            ab_append(&ab, " ", 1); // newline marker, or a wide cluster cut by
                                    // the left edge: pad instead of shifting
          else
            ab_append(&ab, line + j, grapheme_next(line, j) - j);
        }

        col += w;
        if (at_end)
          break;

        int n = grapheme_next(line, j);
        if (n <= j)
          break;
        j = n;
      }
      if (rev)
        ab_append(&ab, "\x1b[27m", 5);
    } else {
      ab_append(&ab, "~", 1);
    }
    ab_append(&ab, "\r\n", 2);
  }

  // --- Bottom status bar ---
  ab_append(&ab, "\x1b[30;103m", 9); // black text on bright yellow
  char bot[512];
  int bn = snprintf(bot, sizeof bot, " %d:%d  lines %d",
                    e->cy + 1, cursor_col(e) + 1, e->buf.nlines);
  if (bn > e->cols)
    bn = e->cols;
  ab_append(&ab, bot, bn);
  for (int i = bn; i < e->cols; i++)
    ab_append(&ab, " ", 1);
  ab_append(&ab, "\x1b[m", 3);

  // --- Draw hooks of the registered modules ---
  dispatch_draw(e, &ab);

  // --- Position the real cursor ---
  int sy = (e->cy - e->rowoff) + 2; // +1 status bar, +1 one-based
  int sx = (cursor_col(e) - e->coloff) + gutter + 1;
  int cn = snprintf(tmp, sizeof tmp, "\x1b[%d;%dH", sy, sx);
  ab_append(&ab, tmp, cn);

  ab_append(&ab, "\x1b[?25h", 6); // show the cursor

  WRITE(1, ab.b, ab.len);
  ab_free(&ab);
}

void screen_clear(void) {
  WRITE(1, "\x1b[2J\x1b[H", 7);
}
