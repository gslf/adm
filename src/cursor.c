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

// One screenful of text, the step a page key takes.
static int page(const editor *e) {
  int th = e->rows - 2; // two status bars
  return th > 0 ? th : 1;
}

static void move_page_up(editor *e) {
  int n = page(e);
  e->cy = e->cy > n ? e->cy - n : 0;

  // Scroll with the cursor so it keeps its place on the screen.
  e->rowoff = e->rowoff > n ? e->rowoff - n : 0;
  seek_column(e);
}

static void move_page_down(editor *e) {
  int n = page(e);
  int last = e->buf.nlines > 0 ? e->buf.nlines - 1 : 0;

  e->cy += n;
  if (e->cy > last)
    e->cy = last;

  e->rowoff += n;
  if (e->rowoff > last)
    e->rowoff = last;

  seek_column(e);
}

static void move_home(editor *e) {
  e->cx = 0;
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
  e->sel_mode = 0;
}

void cursor_select_toggle(editor *e) {
  if (e->sel_mode) {
    selection_clear(e);
    return;
  }

  e->sel_mode = 1;
  e->selx = e->cx;
  e->sely = e->cy;
  e->sel_active = 0; // nothing is covered until the cursor moves
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
    // Cut the tail of the first row and the head of the last one, drop the
    // fully selected rows in between, then pull what is left of the last
    // row up onto the first.
    char *first = buffer_line(&e->buf, sr);
    int flen = first ? (int)strlen(first) : 0;
    for (int i = flen - sc; i > 0; i--)
      buffer_delete_char(&e->buf, sr, sc);
    for (int i = ec; i > 0; i--)
      buffer_delete_char(&e->buf, er, 0);
    for (int r = er - 1; r > sr; r--)
      buffer_remove_line(&e->buf, r);
    buffer_join_line(&e->buf, sr);
  }

  e->cy = sr;
  e->cx = sc;
  selection_clear(e); // the text it covered is gone, and so is the mode
  cursor_mark_column(e);
}

// Every cursor command goes through here. With selection mode off a move
// drops the selection; with it on, the anchor stays put and the move extends
// the selection to wherever the cursor lands.
static void do_move(editor *e, command move) {
  if (!e->sel_mode)
    e->sel_active = 0;

  move(e);

  // The cursor can sit back on the anchor, either by shrinking the selection
  // down to nothing or by never leaving in the first place, which is what
  // happens against the top or the bottom of the buffer. A selection that
  // covers no text has to stop counting as one: while it is still active,
  // backspace and delete believe there is something to remove, do nothing,
  // and swallow the keystroke while marking the file as modified.
  if (e->sel_mode)
    e->sel_active = (e->cy != e->sely || e->cx != e->selx);
}

void cursor_up(editor *e)         { do_move(e, move_up); }
void cursor_down(editor *e)       { do_move(e, move_down); }
void cursor_left(editor *e)       { do_move(e, move_left); }
void cursor_right(editor *e)      { do_move(e, move_right); }
void cursor_word_left(editor *e)  { do_move(e, move_word_left); }
void cursor_word_right(editor *e) { do_move(e, move_word_right); }
void cursor_page_up(editor *e)    { do_move(e, move_page_up); }
void cursor_page_down(editor *e)  { do_move(e, move_page_down); }
void cursor_home(editor *e)       { do_move(e, move_home); }

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
