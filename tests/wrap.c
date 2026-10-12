#include "dispatch.h"
#include "cursor.h"
#include "fileio.h"
#include "screen.h"
#include "wrap.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void prefix(editor *e, int key) {
  dispatch_key(e, CTRL('x'));
  assert(e->prefix_active);
  dispatch_key(e, key);
  assert(!e->prefix_active);
}

static void screen_cell(editor *e, int *x, int *y) {
  rect area = layout_content(e, e->view);
  int width = area.width - screen_gutter(e->view);
  cursor_scroll_view(e->view, width, area.height, e->wrap_enabled);
  view_cursor_screen(e->view, width, e->wrap_enabled, x, y);
}

int main(int argc, char **argv) {
  assert(argc == 2);

  // Rows break after the blanks that follow a word.
  const char *words = "hello world";
  assert(wrap_next(words, 0, 6) == 6 && wrap_next(words, 6, 6) == -1);
  assert(wrap_rows(words, 6) == 2 && wrap_rows(words, 12) == 1);
  // The last column only takes a blank or the line end, so words longer
  // than the remaining text columns break between clusters.
  assert(wrap_next(words, 0, 5) == 4 && wrap_next(words, 4, 5) == 6);
  assert(wrap_next(words, 6, 5) == 10 && wrap_next(words, 10, 5) == -1);
  assert(wrap_rows("abcd", 5) == 1 && wrap_rows("abcde", 5) == 2);
  // A blank in the last column pushes the line end onto a row of its own.
  assert(wrap_rows("abcd ", 5) == 2 && wrap_row("abcd ", 5, 5, NULL) == 1);
  // Indentation alone never fills a row.
  assert(wrap_next("    longword", 0, 6) == 5);
  // Wide clusters are never split and a too narrow row still advances.
  assert(wrap_rows("界界界", 4) == 3 && wrap_next("界界界", 0, 4) == 3);
  assert(wrap_rows("界", 1) == 2 && wrap_rows("", 1) == 1);
  int start;
  assert(wrap_row(words, 6, 5, &start) == 0 && start == 0);
  assert(wrap_row(words, 6, 6, &start) == 1 && start == 6);
  assert(wrap_start(words, 6, 1) == 6 && wrap_start(words, 6, 9) == 6);
  assert(wrap_col(words, 6, 9) == 3 && wrap_col("界a", 0, 4) == 3);
  // A row ends on its last cluster; the next row owns the byte after it.
  assert(wrap_byte(words, 0, 6, 50) == 5 && wrap_byte(words, 0, 6, 2) == 2);
  assert(wrap_byte(words, 6, 6, 50) == 11 && wrap_byte("界界界", 3, 4, 1) == 3);

  // Five 7-column words fit each 38-column row of a 40-column terminal.
  char text[512] = "short\n";
  for (int i = 0; i < 30; i++)
    snprintf(text + strlen(text), sizeof text - strlen(text), "word%02d ", i);
  text[strlen(text) - 1] = '\n';
  strcat(text, "end\n");
  assert(file_write(argv[1], text) == 0);
  editor e = {.document.filename = argv[1], .rows = 12, .cols = 40, .running = 1};
  assert(load(&e.document.buf, argv[1]) == 0);
  dispatch_init(&e);
  layout_arrange(&e);
  assert(e.wrap_enabled && e.document.buf.nlines == 3);
  assert(screen_gutter(e.view) == 2);
  const char *line = buffer_line(&e.document.buf, 1);
  assert(wrap_rows(line, 38) == 6 && wrap_start(line, 38, 1) == 35);

  // Up and Down move by visual rows and keep the column inside the row.
  dispatch_key(&e, KEY_DOWN);
  assert(e.view->cy == 1 && e.view->cx == 0);
  dispatch_key(&e, KEY_DOWN);
  assert(e.view->cy == 1 && e.view->cx == 35);
  for (int i = 0; i < 3; i++)
    dispatch_key(&e, KEY_RIGHT);
  assert(e.view->sticky == 3);
  dispatch_key(&e, KEY_DOWN);
  assert(e.view->cy == 1 && e.view->cx == 73);
  dispatch_key(&e, KEY_UP);
  dispatch_key(&e, KEY_UP);
  assert(e.view->cy == 1 && e.view->cx == 3);
  dispatch_key(&e, KEY_UP);
  assert(e.view->cy == 0 && e.view->cx == 3);

  // The cursor at the end of the wrapped line sits on its last row.
  dispatch_key(&e, KEY_DOWN);
  dispatch_key(&e, CTRL('e'));
  int x, y;
  screen_cell(&e, &x, &y);
  assert(e.view->cx == 209 && x == 34 && y == 6);
  dispatch_key(&e, KEY_DOWN);
  assert(e.view->cy == 2 && e.view->cx == 3);
  dispatch_key(&e, KEY_UP);
  assert(e.view->cy == 1 && e.view->cx == 209);

  // Scrolling counts visual rows and can stop inside a wrapped line.
  e.rows = 6;
  layout_arrange(&e);
  assert(layout_content(&e, e.view).height == 4);
  dispatch_key(&e, CTRL('e'));
  screen_cell(&e, &x, &y);
  assert(e.view->rowoff == 1 && e.view->wrapoff == 2 && y == 3);
  for (int i = 0; i < 5; i++)
    dispatch_key(&e, KEY_UP);
  screen_cell(&e, &x, &y);
  assert(e.view->cy == 1 && e.view->rowoff == 1 && e.view->wrapoff == 0 && y == 0);
  dispatch_key(&e, KEY_PGDOWN);
  assert(e.view->cy == 1 && e.view->cx == 174);
  assert(e.view->rowoff == 1 && e.view->wrapoff == 4);
  screen_cell(&e, &x, &y);
  assert(y == 0);
  cursor_screen_bottom(&e);
  assert(e.view->cy == 2);
  dispatch_key(&e, KEY_PGUP);
  assert(e.view->cy == 1 && e.view->cx == 104);
  assert(e.view->rowoff == 1 && e.view->wrapoff == 0);
  // Edits that shorten the top line clamp the rows scrolled out of it.
  e.view->wrapoff = 9;
  screen_cell(&e, &x, &y);
  assert(e.view->wrapoff <= 5);

  // C-x w turns wrapping off everywhere: lines scroll sideways again.
  prefix(&e, 'w');
  assert(!e.wrap_enabled && !strcmp(e.notice, "Word wrap off"));
  dispatch_key(&e, META('<'));
  dispatch_key(&e, KEY_DOWN);
  dispatch_key(&e, CTRL('e'));
  screen_cell(&e, &x, &y);
  assert(e.view->wrapoff == 0 && e.view->coloff > 0 && y == 0);
  dispatch_key(&e, KEY_DOWN);
  assert(e.view->cy == 2);
  prefix(&e, 'w');
  assert(e.wrap_enabled && !strcmp(e.notice, "Word wrap on"));
  screen_cell(&e, &x, &y);
  assert(e.view->coloff == 0);
  dispatch_shutdown(&e);
  puts("Wrap regressions passed.");
  return 0;
}
