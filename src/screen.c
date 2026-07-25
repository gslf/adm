#include "screen.h"
#include "cursor.h"

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
  int tn = snprintf(top, sizeof top, "  %s%s",
                    e->filename ? e->filename : "[No Name]",
                    e->dirty ? " **" : "");
  int avail = e->cols - logolen; // room left after the logo
  if (avail < 0)
    avail = 0;
  if (tn > avail)
    tn = avail;
  ab_append(&ab, top, tn);
  for (int i = logolen + tn; i < e->cols; i++)
    ab_append(&ab, " ", 1);
  ab_append(&ab, "\x1b[m", 3);
  ab_append(&ab, "\r\n", 2);

  // --- Text area ---
  for (int y = 0; y < th; y++) {
    int filerow = e->rowoff + y;
    ab_append(&ab, "\x1b[K", 3); // clear the line

    if (filerow < e->buf.nlines) {
      int ln = snprintf(tmp, sizeof tmp, "%*d ", gutter - 1, filerow + 1);
      ab_append(&ab, tmp, ln);

      char *line = buffer_line(&e->buf, filerow);
      int len = line ? (int)strlen(line) : 0;
      if (e->coloff < len) {
        int avail = len - e->coloff;
        if (avail > tw)
          avail = tw;
        ab_append(&ab, line + e->coloff, avail);
      }
    } else {
      ab_append(&ab, "~", 1);
    }
    ab_append(&ab, "\r\n", 2);
  }

  // --- Bottom status bar ---
  ab_append(&ab, "\x1b[30;103m", 9); // black text on bright yellow
  char bot[512];
  int bn = snprintf(bot, sizeof bot, " %d:%d  lines %d",
                    e->cy + 1, e->cx + 1, e->buf.nlines);
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
  int sx = (e->cx - e->coloff) + gutter + 1;
  int cn = snprintf(tmp, sizeof tmp, "\x1b[%d;%dH", sy, sx);
  ab_append(&ab, tmp, cn);

  ab_append(&ab, "\x1b[?25h", 6); // show the cursor

  WRITE(1, ab.b, ab.len);
  ab_free(&ab);
}

void screen_clear(void) {
  WRITE(1, "\x1b[2J\x1b[H", 7);
}
