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

// How selected text is painted. Deliberately a colour pair and not reverse
// video: the terminal draws its own cursor by inverting the cell it sits on,
// so reverse video would cancel out there and that one character would look
// unselected. It is always the character under the cursor, which on a
// selection made backwards is the first one of the run, so selecting towards
// the start of a line left its first character looking untouched.
// With real colours the cursor still inverts its cell, but the result reads
// as a caret sitting inside the selection instead of a hole in it.
#define SELECT_ON "\x1b[44;97m"  // white on blue
#define SELECT_OFF "\x1b[49;39m" // back to the terminal's own colours

// The two status bars, and the warning that replaces the bottom one while a
// quit is waiting to be confirmed. Red rather than the usual yellow, because
// answering it wrongly is the one keystroke in the editor that cannot be
// taken back.
#define STATUS_COLOURS "\x1b[30;103m"  // black on bright yellow
#define WARNING_COLOURS "\x1b[97;41m"  // white on red
#define QUIT_WARNING "Unsaved changes will be lost - press Ctrl-Q again to quit"

// The badge the bottom bar starts with while selection mode is on, painted in
// the same blue as the selected text so the two read as one thing. Bold, and
// dropped back to normal weight before the bar's own colours return.
#define SELECT_BADGE "\x1b[97;44;1m SELECT \x1b[22m"
#define SELECT_BADGE_COLS 8 // visible width of " SELECT "

static void append_str(abuf *ab, const char *s) {
  ab_append(ab, s, (int)strlen(s));
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
  append_str(&ab, STATUS_COLOURS);
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
      int painted = 0; // selection colours currently on
      int col = 0;     // column of the cluster about to be drawn
      for (int j = 0; ; ) {
        int inside = (j >= from && j < to);
        int at_end = (j >= len);
        if (at_end && !inside)
          break; // past the text and outside the selection

        int w = at_end ? 1 : grapheme_width(line, j);
        if (col + w > e->coloff + tw)
          break; // would spill past the right edge

        if (col >= e->coloff || col + w > e->coloff) {
          if (inside != painted) {
            append_str(&ab, inside ? SELECT_ON : SELECT_OFF);
            painted = inside;
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
      if (painted)
        append_str(&ab, SELECT_OFF);
    } else {
      ab_append(&ab, "~", 1);
    }
    ab_append(&ab, "\r\n", 2);
  }

  // --- Bottom status bar ---
  // A quit waiting to be confirmed takes the whole bar over, in its own
  // colours: until the user answers it, nothing else down here matters.
  char bot[512];
  int used = 0; // visible columns the badge has already taken
  if (e->quit_pending) {
    append_str(&ab, WARNING_COLOURS);
    snprintf(bot, sizeof bot, " %s", QUIT_WARNING);
  } else {
    append_str(&ab, STATUS_COLOURS);

    // Selection mode announces itself first thing on the bar: the mode is
    // never on without the screen saying so, even before the first move.
    if (e->sel_mode || e->sel_active) {
      append_str(&ab, SELECT_BADGE);
      append_str(&ab, STATUS_COLOURS);
      used = SELECT_BADGE_COLS;
    }

    // Characters are grapheme clusters, the same thing the cursor steps
    // over, so what the bar counts is what the user would count by hand.
    int bl = snprintf(bot, sizeof bot, " %d:%d  lines %d  chars %d",
                      e->cy + 1, cursor_col(e) + 1, e->buf.nlines,
                      buffer_char_count(&e->buf));
    if (e->sel_active && bl > 0 && bl < (int)sizeof bot)
      snprintf(bot + bl, sizeof bot - bl, "  sel %d",
               selection_char_count(e));
  }

  // strlen and not what snprintf returned: that is the length the text would
  // have had, which on a narrow terminal runs past the end of the buffer.
  int bn = (int)strlen(bot);
  if (bn > e->cols - used)
    bn = e->cols - used;
  if (bn < 0)
    bn = 0;
  ab_append(&ab, bot, bn);
  for (int i = used + bn; i < e->cols; i++)
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
