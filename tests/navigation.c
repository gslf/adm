#include "dispatch.h"
#include "navigation.h"
#include "search.h"
#include "screen.h"
#include "fileio.h"
#include "cursor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void prefix(editor *e, int key) {
  dispatch_key(e, CTRL('x'));
  assert(e->prefix_active);
  dispatch_key(e, key);
  assert(!e->prefix_active);
}
static void at_line(editor *e, int pane, int line) {
  if (e->windows.active != pane) fprintf(stderr, "line %d: expected %d, got %d\n", line, pane, e->windows.active);
  assert(e->windows.active == pane && e->view == &e->windows.panes[pane]);
}
#define at(e, pane) at_line(e, pane, __LINE__)
int main(int argc, char **argv) {
  assert(argc == 2);
  assert(file_write(argv[1], "abc\ndef\n") == 0);
  editor e = {.document.filename = argv[1], .rows = 40, .cols = 160, .running = 1};
  assert(load(&e.document.buf, argv[1]) == 0);
  dispatch_register(search_module());
  dispatch_init(&e);
  const char *removed = "oO0123npwTv";
  for (; *removed; removed++) assert(!dispatch_prefix_find(&e, *removed));
  assert(dispatch_prefix_find(&e, 'l') && dispatch_prefix_find(&e, 'f') &&
         dispatch_prefix_find(&e, 'g') && dispatch_prefix_find(&e, 'k'));
  prefix(&e, 'f'); assert(e.sidebar.focused && e.sidebar.kind == SIDEBAR_FILES);
  prefix(&e, 'f'); assert(e.sidebar.focused); // Open is idempotent.
  dispatch_key(&e, CTRL('l')); assert(!e.sidebar.focused); at(&e, 0);
  const char *directions = "hjkl";
  for (const char *key = directions; *key; key++) {
    dispatch_key(&e, CTRL(*key)); at(&e, 0);
  }
  assert(!navigation_quick_key(CTRL('s')));
  dispatch_key(&e, CTRL('b'));
  dispatch_key(&e, CTRL('u')); at(&e, 1); assert(e.sidebar.focused && !e.view->doc);
  layout_set_document(&e, &e.document);
  dispatch_key(&e, CTRL('l')); // Return to the right editor pane.
  dispatch_key(&e, CTRL('q')); at(&e, 2);
  layout_set_document(&e, &e.document);
  dispatch_key(&e, CTRL('l'));
  dispatch_key(&e, CTRL('h')); at(&e, 0);
  prefix(&e, 'q'); at(&e, 3);
  layout_set_document(&e, &e.document);
  dispatch_key(&e, CTRL('l'));
  assert(e.document.views == 4);
  // Grid: 0 1 / 3 2. All four directions wrap within the row/column.
  prefix(&e, 'm'); assert(e.move_active);
  dispatch_key(&e, 'l'); at(&e, 2);
  dispatch_key(&e, 'l'); at(&e, 3);
  dispatch_key(&e, 'k'); at(&e, 0);
  dispatch_key(&e, 'k'); at(&e, 3);
  dispatch_key(&e, 'h'); at(&e, 2);
  dispatch_key(&e, 'h'); at(&e, 3);
  dispatch_key(&e, 'j'); at(&e, 0);
  dispatch_key(&e, 'j'); at(&e, 3);
  dispatch_key(&e, 's'); assert(!e.sidebar.focused); at(&e, 3);
  dispatch_key(&e, 'b'); assert(e.sidebar.focused);
  dispatch_key(&e, 'h'); assert(e.sidebar.focused);
  dispatch_key(&e, 'j'); assert(e.sidebar.focused);
  dispatch_key(&e, 'l'); assert(!e.sidebar.focused); at(&e, 3);
  const int ignored[] = {'X', '\r', CTRL('d'), CTRL('x'), '2', '?'};
  for (unsigned i = 0; i < sizeof ignored / sizeof *ignored; i++) dispatch_key(&e, ignored[i]);
  assert(!strcmp(buffer_line(&e.document.buf, 0), "abc") && !e.document.dirty);
  e.cols = 1; e.rows = 1; layout_arrange(&e); assert(e.windows.compact);
  dispatch_key(&e, 'l'); at(&e, 2);
  dispatch_key(&e, 'k'); at(&e, 1);
  dispatch_key(&e, 'h'); at(&e, 0);
  dispatch_key(&e, 'h'); at(&e, 1);
  dispatch_key(&e, 'l'); at(&e, 0);
  dispatch_key(&e, 'k'); at(&e, 3);
  dispatch_key(&e, 'j'); at(&e, 0);
  dispatch_key(&e, '\x1b'); assert(!e.move_active);
  e.cols = 160; e.rows = 40; layout_arrange(&e);
  e.view->cx = 1; e.view->sel_active = e.view->sel_mode = 1;
  e.view->selx = 0; e.view->sely = 0;
  dispatch_key(&e, CTRL('l')); at(&e, 1);
  dispatch_key(&e, CTRL('j')); at(&e, 2);
  dispatch_key(&e, CTRL('h')); at(&e, 3);
  dispatch_key(&e, CTRL('k')); at(&e, 0);
  dispatch_key(&e, CTRL('k')); at(&e, 3);
  dispatch_key(&e, CTRL('j')); at(&e, 0);
  dispatch_key(&e, CTRL('h')); at(&e, 1);
  dispatch_key(&e, CTRL('l')); at(&e, 0);
  assert(e.view->cx == 1 && e.view->sel_active && !e.move_active);
  selection_clear(&e);
  // Resize ratios and mixed T-shaped layouts keep spatial meaning.
  assert(layout_resize(&e, LAYOUT_VERTICAL, 5));
  assert(layout_resize(&e, LAYOUT_HORIZONTAL, 5));
  dispatch_key(&e, CTRL('l')); at(&e, 1);
  prefix(&e, 'c'); assert(e.windows.count == 3 && e.document.views == 3);
  dispatch_key(&e, CTRL('h')); at(&e, 0);
  dispatch_key(&e, CTRL('j')); at(&e, 3);
  dispatch_key(&e, CTRL('k')); at(&e, 0);
  dispatch_key(&e, CTRL('l')); at(&e, 2);
  dispatch_key(&e, CTRL('j')); at(&e, 2);
  // MOVE persists across tab switches and cannot accidentally edit a buffer.
  unsigned long first = e.active_tab->id;
  dispatch_key(&e, CTRL('t')); assert(!e.view->doc && e.sidebar.focused);
  unsigned long second = e.active_tab->id;
  prefix(&e, 'm'); dispatch_key(&e, 'p'); assert(e.active_tab->id == first && e.move_active);
  dispatch_key(&e, 'n'); assert(e.active_tab->id == second && e.move_active && !e.sidebar.focused);
  dispatch_key(&e, 'n'); assert(e.active_tab->id == first);
  dispatch_key(&e, 'p'); assert(e.active_tab->id == second);
  dispatch_key(&e, 'b'); assert(e.sidebar.focused);
  dispatch_key(&e, '\x1b'); assert(!e.move_active && e.sidebar.focused);
  prefix(&e, 'N'); dispatch_key(&e, 'a'); dispatch_key(&e, CTRL('h'));
  dispatch_key(&e, CTRL('n')); dispatch_key(&e, CTRL('p'));
  dispatch_key(&e, CTRL('q')); dispatch_key(&e, CTRL('u')); dispatch_key(&e, CTRL('t'));
  assert(e.new_file.active && !e.new_file.name[0] && e.active_tab->id == second);
  dispatch_key(&e, '\x1b');
  prefix(&e, 'g'); assert(e.sidebar.kind == SIDEBAR_GIT && e.sidebar.focused);
  // Isolate prompt input from asynchronous repository discovery.
  git_panel_shutdown(&e); git_panel_init(&e);
  e.git.mode = GIT_MESSAGE;
  dispatch_key(&e, 'A'); assert(e.git.input_length == 1);
  dispatch_key(&e, CTRL('b'));
  assert(e.git.input_cursor == 0 && e.sidebar.focused);
  dispatch_key(&e, KEY_END); dispatch_key(&e, CTRL('h'));
  dispatch_key(&e, CTRL('p')); dispatch_key(&e, CTRL('n')); dispatch_key(&e, CTRL('k'));
  dispatch_key(&e, CTRL('q')); dispatch_key(&e, CTRL('u')); dispatch_key(&e, CTRL('t'));
  assert(e.git.mode == GIT_MESSAGE && !e.git.input_length && e.active_tab->id == second);
  dispatch_key(&e, '\x1b'); assert(e.git.mode == GIT_FILES);
  layout_set_document(&e, &e.document);
  dispatch_key(&e, CTRL('l'));
  prefix(&e, 'l'); dispatch_key(&e, '2'); dispatch_key(&e, CTRL('n'));
  assert(e.active_tab->id == second); dispatch_key(&e, '\r'); assert(e.view->cy == 1);
  dispatch_key(&e, CTRL('s')); dispatch_key(&e, 'a'); dispatch_key(&e, CTRL('h'));
  dispatch_key(&e, CTRL('p')); assert(e.active_tab->id == second);
  dispatch_key(&e, '\x1b');
  prefix(&e, '?'); assert(e.help_active);
  abuf ab = {0}; dispatch_draw(&e, &ab); ab_append(&ab, "\0", 1);
  assert(strstr(ab.b, "ESC returns to editing") && strstr(ab.b, "C-x k")); ab_free(&ab);
  dispatch_key(&e, CTRL('n')); assert(e.active_tab->id == second && e.help_active);
  e.rows = 3; e.cols = 1; dispatch_key(&e, KEY_END);
  assert(e.help_scroll > 0); dispatch_draw(&e, &ab); ab_free(&ab);
  e.rows = 80; e.cols = 160; dispatch_draw(&e, &ab); ab_free(&ab); assert(!e.help_scroll);
  dispatch_key(&e, '\x1b'); assert(!e.help_active);
  prefix(&e, 'm');
  dispatch_key(&e, CTRL('q'));
  assert(e.windows.count == 2 && !e.view->doc && e.move_active);
  dispatch_key(&e, CTRL('u'));
  assert(e.windows.count == 3 && !e.view->doc && e.move_active);
  dispatch_key(&e, CTRL('t'));
  assert(e.tab_count == 3 && e.move_active);
  dispatch_key(&e, '\x1b');
  prefix(&e, 'k');
  // The empty quick splits add no document references.
  dispatch_key(&e, CTRL('l'));
  dispatch_key(&e, CTRL('k'));
  assert(e.view->doc == &e.document);
  // Closing all references in one tab prompts only when there is no survivor.
  dispatch_key(&e, 'Q'); assert(e.document.dirty);
  prefix(&e, 'k'); assert(e.tab_count == 1 && !e.confirmation && e.document.views == 3);
  prefix(&e, 't'); // Empty surviving tab.
  dispatch_key(&e, CTRL('p')); assert(e.active_tab->id == first);
  int active = e.windows.active;
  prefix(&e, 'k'); assert(e.confirmation && e.tab_count == 2);
  dispatch_key(&e, CTRL('n')); // Confirmation owns this key; it cancels, never navigates.
  assert(!e.confirmation && e.active_tab->id == first && e.windows.active == active && e.document.views == 3);
  prefix(&e, 'k'); dispatch_key(&e, 'y');
  assert(e.tab_count == 1 && !e.view->doc && !e.document.views && !e.document.buf.head);
  dispatch_shutdown(&e);
  puts("Navigation regressions passed.");
  return 0;
}
