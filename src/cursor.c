#include "cursor.h"
#include "screen.h"
#include "utf8.h"

#include <string.h>
#include <stdio.h>

// cx and cy are byte offsets into the line, always kept on a grapheme
// cluster boundary. The screen column is derived from them, never stored.

static int line_len(editor *e, int row) {
  char *l = buffer_line(&e->view->doc->buf, row);
  return l ? (int)strlen(l) : 0;
}

int view_cursor_col(const view *v) {
  if (!v->doc)
    return 0;
  const char *line = buffer_line(&v->doc->buf, v->cy);
  return line ? utf8_cols(line, v->cx) : 0;
}

int cursor_col(const editor *e) {
  return view_cursor_col(e->view);
}

void cursor_mark_column(editor *e) {
  e->view->sticky = cursor_col(e);
}

// Put the cursor on the sticky column of the current line, or at its end if
// the line is too short. This is what makes a run of up and down keys come
// back to the column it started from instead of drifting left.
static void seek_column(editor *e) {
  char *line = buffer_line(&e->view->doc->buf, e->view->cy);
  e->view->cx = line ? utf8_byte_at_col(line, e->view->sticky) : 0;
}

// --- Raw movement, selection unaware ---

static void move_up(editor *e) {
  if (e->view->cy > 0)
    e->view->cy--;
  seek_column(e);
}

static void move_down(editor *e) {
  if (e->view->cy < e->view->doc->buf.nlines - 1)
    e->view->cy++;
  seek_column(e);
}

static void move_left(editor *e) {
  char *line = buffer_line(&e->view->doc->buf, e->view->cy);
  if (e->view->cx > 0) {
    e->view->cx = line ? grapheme_prev(line, e->view->cx) : 0;
  } else if (e->view->cy > 0) {
    // At line start: move to the end of the previous line.
    e->view->cy--;
    e->view->cx = line_len(e, e->view->cy);
  }
  cursor_mark_column(e);
}

static void move_right(editor *e) {
  char *line = buffer_line(&e->view->doc->buf, e->view->cy);
  if (e->view->cx < line_len(e, e->view->cy)) {
    e->view->cx = line ? grapheme_next(line, e->view->cx) : 0;
  } else if (e->view->cy < e->view->doc->buf.nlines - 1) {
    // At line end: move to the start of the next line.
    e->view->cy++;
    e->view->cx = 0;
  }
  cursor_mark_column(e);
}

// One screenful of text, the step a page key takes.
static int page(const editor *e) {
  int th = layout_content(e, e->view).height;
  return th > 0 ? th : 1;
}

static void move_page_up(editor *e) {
  int n = page(e);
  e->view->cy = e->view->cy > n ? e->view->cy - n : 0;

  // Scroll with the cursor so it keeps its place on the screen.
  e->view->rowoff = e->view->rowoff > n ? e->view->rowoff - n : 0;
  seek_column(e);
}

static void move_page_down(editor *e) {
  int n = page(e);
  int last = e->view->doc->buf.nlines > 0 ? e->view->doc->buf.nlines - 1 : 0;

  e->view->cy += n;
  if (e->view->cy > last)
    e->view->cy = last;

  e->view->rowoff += n;
  if (e->view->rowoff > last)
    e->view->rowoff = last;

  seek_column(e);
}

static void move_home(editor *e) {
  e->view->cx = 0;
  cursor_mark_column(e);
}

static void move_end(editor *e) {
  e->view->cx = line_len(e, e->view->cy);
  cursor_mark_column(e);
}

static void move_file_start(editor *e) {
  e->view->cy = 0;
  e->view->cx = 0;
  cursor_mark_column(e);
}

static void move_file_end(editor *e) {
  e->view->cy = e->view->doc->buf.nlines > 0 ? e->view->doc->buf.nlines - 1 : 0;
  e->view->cx = line_len(e, e->view->cy);
  cursor_mark_column(e);
}

// The last line of the file that is on the screen right now. The screen does
// not move, only the cursor drops to the bottom of it, keeping its column.
static void move_screen_bottom(editor *e) {
  int last = e->view->doc->buf.nlines > 0 ? e->view->doc->buf.nlines - 1 : 0;
  int bottom = e->view->rowoff + page(e) - 1;
  e->view->cy = bottom < last ? bottom : last;
  seek_column(e);
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
  char *line = buffer_line(&e->view->doc->buf, e->view->cy);
  int len = line ? (int)strlen(line) : 0;

  if (!line || e->view->cx >= len) {
    move_right(e); // at the line end, carry on to the next line
    return;
  }

  // Leave the current word, then cross the separators to the next one.
  while (e->view->cx < len && is_word(line, e->view->cx))
    e->view->cx = grapheme_next(line, e->view->cx);
  while (e->view->cx < len && !is_word(line, e->view->cx))
    e->view->cx = grapheme_next(line, e->view->cx);

  cursor_mark_column(e);
}

static void move_word_left(editor *e) {
  char *line = buffer_line(&e->view->doc->buf, e->view->cy);

  if (!line || e->view->cx == 0) {
    move_left(e); // at the line start, carry on to the previous line
    return;
  }

  // Step back over the separators, then to the front of the word.
  e->view->cx = grapheme_prev(line, e->view->cx);
  while (e->view->cx > 0 && !is_word(line, e->view->cx))
    e->view->cx = grapheme_prev(line, e->view->cx);
  while (e->view->cx > 0) {
    int p = grapheme_prev(line, e->view->cx);
    if (!is_word(line, p))
      break;
    e->view->cx = p;
  }

  cursor_mark_column(e);
}

// --- Selection ---

void selection_clear(editor *e) {
  e->view->sel_active = 0;
  e->view->sel_mode = 0;
}

void cursor_select_toggle(editor *e) {
  if (!e->view->doc)
    return;
  if (e->view->sel_mode) {
    selection_clear(e);
    return;
  }

  e->view->sel_mode = 1;
  e->view->selx = e->view->cx;
  e->view->sely = e->view->cy;
  e->view->sel_active = 0; // nothing is covered until the cursor moves
}

int view_selection_range(const view *v, int *sr, int *sc, int *er, int *ec) {
  if (!v->doc || !v->sel_active)
    return 0;

  // The anchor may sit after the cursor, so order the two points.
  if (v->sely < v->cy || (v->sely == v->cy && v->selx <= v->cx)) {
    *sr = v->sely; *sc = v->selx;
    *er = v->cy;   *ec = v->cx;
  } else {
    *sr = v->cy;   *sc = v->cx;
    *er = v->sely; *ec = v->selx;
  }
  return 1;
}

int selection_range(const editor *e, int *sr, int *sc, int *er, int *ec) {
  return view_selection_range(e->view, sr, sc, er, ec);
}

int selection_delete(editor *e) {
  int sr, sc, er, ec;
  if (!selection_range(e, &sr, &sc, &er, &ec)) return 0;
  if (buffer_replace_span(&e->view->doc->buf, sr, sc, er, ec, "", 0, NULL) < 0) {
    snprintf(e->notice, sizeof e->notice, "Cannot delete selection; text unchanged");
    return -1;
  }
  e->view->cy = sr; e->view->cx = sc;
  selection_clear(e);
  cursor_mark_column(e);
  return 1;
}

int selection_char_count(const editor *e) {
  int sr, sc, er, ec;
  if (!selection_range(e, &sr, &sc, &er, &ec))
    return 0;

  int total = 0;
  for (int r = sr; r <= er; r++) {
    const char *line = buffer_line(&e->view->doc->buf, r);
    if (!line)
      continue;

    int from = (r == sr) ? sc : 0;
    int to = (r == er) ? ec : (int)strlen(line);
    for (int j = from; j < to; j = grapheme_next(line, j))
      total++;
    if (r < er)
      total++; // the newline the selection runs over
  }
  return total;
}

// Every cursor command goes through here. With selection mode off a move
// drops the selection; with it on, the anchor stays put and the move extends
// the selection to wherever the cursor lands.
static void do_move(editor *e, command move) {
  if (!e->view->doc)
    return;
  if (!e->view->sel_mode)
    e->view->sel_active = 0;

  move(e);

  // The cursor can sit back on the anchor, either by shrinking the selection
  // down to nothing or by never leaving in the first place, which is what
  // happens against the top or the bottom of the buffer. A selection that
  // covers no text has to stop counting as one: while it is still active,
  // backspace and delete believe there is something to remove, do nothing,
  // and swallow the keystroke while marking the file as modified.
  if (e->view->sel_mode)
    e->view->sel_active = (e->view->cy != e->view->sely || e->view->cx != e->view->selx);
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
void cursor_end(editor *e)        { do_move(e, move_end); }
void cursor_file_start(editor *e) { do_move(e, move_file_start); }
void cursor_file_end(editor *e)   { do_move(e, move_file_end); }
void cursor_screen_bottom(editor *e) { do_move(e, move_screen_bottom); }

void cursor_scroll_view(view *v, int width, int height) {
  int th = height > 0 ? height : 1;
  int tw = width > 0 ? width : 1;
  if (v->cy < v->rowoff)
    v->rowoff = v->cy;
  if (v->cy >= v->rowoff + th)
    v->rowoff = v->cy - th + 1;
  int col = view_cursor_col(v);
  if (col < v->coloff)
    v->coloff = col;
  if (col >= v->coloff + tw)
    v->coloff = col - tw + 1;
}

void cursor_scroll(editor *e) {
  rect area = layout_content(e, e->view);
  cursor_scroll_view(e->view, area.width - screen_gutter(e->view), area.height);
}
