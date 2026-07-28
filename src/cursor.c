#include "cursor.h"
#include "screen.h"
#include "utf8.h"

#include <string.h>

// cx and cy are byte offsets into the line, always kept on a grapheme
// cluster boundary. The screen column is derived from them, never stored.

static int line_len(editor *e, int row) {
  char *l = buffer_line(&e->buf, row);
  return l ? (int)strlen(l) : 0;
}

int cursor_col(const editor *e) {
  char *line = buffer_line(&e->buf, e->cy);
  return line ? utf8_cols(line, e->cx) : 0;
}

void cursor_mark_column(editor *e) {
  e->sticky = cursor_col(e);
}

// Put the cursor on the sticky column of the current line, or at its end if
// the line is too short. This is what makes a run of up and down keys come
// back to the column it started from instead of drifting left.
static void seek_column(editor *e) {
  char *line = buffer_line(&e->buf, e->cy);
  e->cx = line ? utf8_byte_at_col(line, e->sticky) : 0;
}

// --- Raw movement, selection unaware ---

static void move_up(editor *e) {
  if (e->cy > 0)
    e->cy--;
  seek_column(e);
}

static void move_down(editor *e) {
  if (e->cy < e->buf.nlines - 1)
    e->cy++;
  seek_column(e);
}

static void move_left(editor *e) {
  char *line = buffer_line(&e->buf, e->cy);
  if (e->cx > 0) {
    e->cx = line ? grapheme_prev(line, e->cx) : 0;
  } else if (e->cy > 0) {
    // At line start: move to the end of the previous line.
    e->cy--;
    e->cx = line_len(e, e->cy);
  }
  cursor_mark_column(e);
}

static void move_right(editor *e) {
  char *line = buffer_line(&e->buf, e->cy);
  if (e->cx < line_len(e, e->cy)) {
    e->cx = line ? grapheme_next(line, e->cx) : 0;
  } else if (e->cy < e->buf.nlines - 1) {
    // At line end: move to the start of the next line.
    e->cy++;
    e->cx = 0;
  }
  cursor_mark_column(e);
}

// A word is a run of letters, digits and underscores. Every byte outside
// ASCII counts as a word byte too, so that words in other scripts hold
// together instead of breaking at each accent.
static int is_word(const char *s, int i) {
  unsigned char c = (unsigned char)s[i];
  if (c >= 0x80)
    return 1;
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

static void move_word_right(editor *e) {
  char *line = buffer_line(&e->buf, e->cy);
  int len = line ? (int)strlen(line) : 0;

  if (!line || e->cx >= len) {
    move_right(e); // at the line end, carry on to the next line
    return;
  }

  // Leave the current word, then cross the separators to the next one.
  while (e->cx < len && is_word(line, e->cx))
    e->cx = grapheme_next(line, e->cx);
  while (e->cx < len && !is_word(line, e->cx))
    e->cx = grapheme_next(line, e->cx);

  cursor_mark_column(e);
}

static void move_word_left(editor *e) {
  char *line = buffer_line(&e->buf, e->cy);

  if (!line || e->cx == 0) {
    move_left(e); // at the line start, carry on to the previous line
    return;
  }

  // Step back over the separators, then to the front of the word.
  e->cx = grapheme_prev(line, e->cx);
  while (e->cx > 0 && !is_word(line, e->cx))
    e->cx = grapheme_prev(line, e->cx);
  while (e->cx > 0) {
    int p = grapheme_prev(line, e->cx);
    if (!is_word(line, p))
      break;
    e->cx = p;
  }

  cursor_mark_column(e);
}

// --- Selection ---

void selection_clear(editor *e) {
  e->sel_active = 0;
}

int selection_range(const editor *e, int *sr, int *sc, int *er, int *ec) {
  if (!e->sel_active)
    return 0;

  // The anchor may sit after the cursor, so order the two points.
  if (e->sely < e->cy || (e->sely == e->cy && e->selx <= e->cx)) {
    *sr = e->sely; *sc = e->selx;
    *er = e->cy;   *ec = e->cx;
  } else {
    *sr = e->cy;   *sc = e->cx;
    *er = e->sely; *ec = e->selx;
  }
  return 1;
}

void selection_delete(editor *e) {
  int sr, sc, er, ec;
  if (!selection_range(e, &sr, &sc, &er, &ec))
    return;

  if (sr == er) {
    for (int i = ec - sc; i > 0; i--)
      buffer_delete_char(&e->buf, sr, sc);
  } else {
    // Cut the tail of the first row and the head of the last one, then fold
    // every row in between away by joining them onto the first.
    char *first = buffer_line(&e->buf, sr);
    int flen = first ? (int)strlen(first) : 0;
    for (int i = flen - sc; i > 0; i--)
      buffer_delete_char(&e->buf, sr, sc);
    for (int i = ec; i > 0; i--)
      buffer_delete_char(&e->buf, er, 0);
    for (int r = er; r > sr; r--)
      buffer_join_line(&e->buf, sr);
  }

  e->cy = sr;
  e->cx = sc;
  e->sel_active = 0;
  cursor_mark_column(e);
}

// A plain move drops the selection.
static void plain(editor *e, command move) {
  e->sel_active = 0;
  move(e);
}

// A shift move drops the anchor on the first step, then moves the cursor.
static void extend(editor *e, command move) {
  if (!e->sel_active) {
    e->selx = e->cx;
    e->sely = e->cy;
    e->sel_active = 1;
  }
  move(e);
}

void cursor_up(editor *e)         { plain(e, move_up); }
void cursor_down(editor *e)       { plain(e, move_down); }
void cursor_left(editor *e)       { plain(e, move_left); }
void cursor_right(editor *e)      { plain(e, move_right); }
void cursor_word_left(editor *e)  { plain(e, move_word_left); }
void cursor_word_right(editor *e) { plain(e, move_word_right); }

void cursor_select_up(editor *e)    { extend(e, move_up); }
void cursor_select_down(editor *e)  { extend(e, move_down); }
void cursor_select_left(editor *e)  { extend(e, move_left); }
void cursor_select_right(editor *e) { extend(e, move_right); }

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

  // Horizontal, in screen columns rather than bytes
  int col = cursor_col(e);
  if (col < e->coloff)
    e->coloff = col;
  if (col >= e->coloff + tw)
    e->coloff = col - tw + 1;
}
