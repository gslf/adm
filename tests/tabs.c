#include "shared_views.h"
#include "dispatch.h"
#include "fileio.h"
#include "cursor.h"
#include "screen.h"
#include "path.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

static void prefix(editor *e, int key) {
  test_prefix_shared(e, key);
}

static void validate(editor *e) {
  int tabs = 0;
  editor_tab *previous = NULL;
  for (editor_tab *tab = e->tabs; tab; tab = tab->next) {
    assert(tab->previous == previous && tab->id);
    workspace *w = tabs_workspace(e, tab);
    assert(w->count >= 1 && w->count <= MAX_PANES && w->panes[w->active].doc);
    int panes = 0;
    for (int i = 0; i < MAX_PANES; i++) {
      document *doc = w->panes[i].doc;
      if (!doc)
        continue;
      panes++;
      int refs = 0;
      for (editor_tab *other = e->tabs; other; other = other->next)
        for (int j = 0; j < MAX_PANES; j++)
          refs += tabs_workspace(e, other)->panes[j].doc == doc;
      assert(doc->views == refs);
    }
    assert(panes == w->count);
    previous = tab;
    tabs++;
  }
  assert(tabs == e->tab_count);
  assert(e->view == &e->windows.panes[e->windows.active]);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  assert(file_write(argv[1], "abc\ndef\nghi\n") == 0);
  editor e = {.document.filename = argv[1], .rows = 30, .cols = 100, .running = 1};
  assert(load(&e.document.buf, argv[1]) == 0);
  dispatch_init(&e);
  char *root = path_absolute(argv[1]);
  assert(root);
  root[path_name(root) - root] = '\0';
  unsigned long first = e.active_tab->id;
  assert(test_shared_split(&e, LAYOUT_VERTICAL));
  assert(test_shared_split(&e, LAYOUT_HORIZONTAL));
  layout_focus(&e, 1);
  assert(test_shared_split(&e, LAYOUT_HORIZONTAL));
  assert(e.windows.count == 4);
  e.view->cy = 1;
  e.view->cx = 2;
  e.view->rowoff = 1;
  e.view->sel_active = e.view->sel_mode = 1;
  e.view->sely = 2;
  e.view->selx = 3;
  int pane = e.windows.active;
  prefix(&e, 't');
  unsigned long second = e.active_tab->id;
  assert(first != second && e.tab_count == 2 && e.windows.count == 1);
  assert(e.view->cy == 1 && e.view->cx == 2 && !e.view->sel_active);
  assert(e.document.views == 5);
  dispatch_key(&e, META('<'));
  dispatch_key(&e, 'X');
  dispatch_key(&e, '\r');
  workspace *old = tabs_workspace(&e, tabs_find(&e, first));
  assert(old->panes[pane].cy == 2 && old->panes[pane].cx == 2);
  assert(old->panes[pane].sely == 3 && old->panes[pane].selx == 3);
  assert(old->panes[pane].rowoff == 2 && old->panes[pane].sel_active);
  assert(!strcmp(buffer_line(&old->panes[pane].doc->buf, 0), "X"));
  prefix(&e, CTRL('s'));
  assert(!old->panes[pane].doc->dirty);
  documents_open(&e, argv[1], NULL);
  assert(e.view->doc == &e.document && e.document.views == 5);
#ifndef _WIN32
  char alias_path[4096];
  snprintf(alias_path, sizeof alias_path, "%s.link", argv[1]);
  assert(symlink(argv[1], alias_path) == 0);
  documents_open(&e, alias_path, NULL);
  assert(e.view->doc == &e.document && e.document.views == 5);
  unlink(alias_path);
  assert(link(argv[1], alias_path) == 0);
  documents_open(&e, alias_path, NULL);
  assert(e.view->doc == &e.document && e.document.views == 5);
  unlink(alias_path);
#endif
  tabs_focus(&e, -1);
  assert(e.active_tab->id == first && e.windows.count == 4);
  assert(e.windows.active == pane && e.view->sel_active && e.view->rowoff == 2);
  e.cols = 10;
  e.rows = 6;
  layout_arrange(&e);
  assert(e.windows.compact);
  tabs_focus(&e, 1);
  assert(e.active_tab->id == second && !e.windows.compact && e.view->area.width == 10);
  tabs_focus(&e, -1);
  assert(e.windows.compact && e.windows.count == 4);
  e.cols = 100;
  e.rows = 30;
  layout_arrange(&e);
  assert(!e.windows.compact);
  tabs_focus(&e, 1);

  // An inactive dirty document still blocks quit and repository mutations.
  document other = {.filename = argv[1]}, third = {0};
  buffer_init(&other.buf);
  layout_set_document(&e, &other);
  dispatch_key(&e, 'Q');
  tabs_focus(&e, -1);
  prefix(&e, CTRL('c'));
  assert(e.confirmation && e.running);
  dispatch_key(&e, 'n');
  assert(documents_unsaved_in(&e, root));
  tabs_focus(&e, 1);
  assert(test_shared_split(&e, LAYOUT_VERTICAL));
  // Two dirty views removed together must trigger exactly one confirmation.
  prefix(&e, 'k');
  assert(e.confirmation && other.views == 2 && e.tab_count == 2);
  dispatch_key(&e, 'n');
  assert(other.views == 2 && e.tab_count == 2);
  prefix(&e, 'k');
  dispatch_key(&e, 'y');
  assert(other.views == 0 && !other.buf.head && e.tab_count == 1);
  assert(!tabs_find(&e, second) && e.document.views == 4);

  // Keep-only must not warn about dirty buffers retained by another tab.
  prefix(&e, 't');
  second = e.active_tab->id;
  tabs_focus(&e, -1);
  layout_set_document(&e, &third);
  layout_focus(&e, 1);
  dispatch_key(&e, '!');
  layout_focus(&e, -1);
  layout_only(&e);
  assert(!e.confirmation && e.windows.count == 1 && e.document.views == 1);
  assert(e.document.dirty && layout_has_unsaved(&e));

  // A delayed preview updates its original inactive tab without stealing focus.
  documents_preview_at(&e, second, 0, "Late diff", "+added\n", NULL);
  assert(e.active_tab->id == first && e.view->doc == &third);
  assert(!tabs_workspace(&e, tabs_find(&e, second))->panes[0].doc->readonly);
  // The replaced document was its last dirty view, so cancellation preserves it.
  assert(e.confirmation && e.document.views == 1);
  dispatch_key(&e, 'n');
  assert(e.document.views == 1 && !e.open_request.text);
  documents_preview_at(&e, second, 0, "Late diff", "+added\n", NULL);
  dispatch_key(&e, 'y');
  assert(!e.document.views && !e.document.buf.head);
  assert(tabs_workspace(&e, tabs_find(&e, second))->panes[0].doc->readonly);
  validate(&e);

  // Repeated insertion/removal includes closing the embedded initial tab.
  tabs_close(&e);
  assert(e.tab_count == 1 && e.active_tab->id == second && !third.views);
  for (int i = 0; i < 500; i++) {
    assert(test_shared_tab(&e));
    assert(test_shared_split(&e, i % 2 ? LAYOUT_VERTICAL : LAYOUT_HORIZONTAL));
    tabs_focus(&e, -1);
    tabs_focus(&e, 1);
    tabs_close(&e);
    assert(!e.confirmation);
    validate(&e);
  }
  // Overflow keeps the active tab visible and sanitizes control characters.
  for (int i = 0; i < 12; i++)
    assert(test_shared_tab(&e));
  abuf ab = {0};
  tabs_draw(&e, &ab, 20);
  ab_append(&ab, "\0", 1);
  assert(strstr(ab.b, "[13/13]") && strstr(ab.b, "13"));
  ab_free(&ab);
  dispatch_shutdown(&e);
  assert(!e.tabs && !e.view && !e.tab_count);

  // Repository reloads reach inactive views, once per document, and preserve dirty text.
  assert(file_write(argv[1], "one\ntwo\nthree\n") == 0);
  editor reload = {.document.filename = argv[1], .rows = 24, .cols = 80, .running = 1};
  assert(load(&reload.document.buf, argv[1]) == 0);
  dispatch_init(&reload);
  reload.view->cy = reload.view->sely = 2;
  reload.view->cx = reload.view->selx = 5;
  reload.view->rowoff = 2;
  assert(test_shared_tab(&reload));
  assert(test_shared_split(&reload, LAYOUT_VERTICAL));
  assert(file_write(argv[1], "fresh\n") == 0);
  documents_reload(&reload, root);
  for (editor_tab *tab = reload.tabs; tab; tab = tab->next) {
    workspace *w = tabs_workspace(&reload, tab);
    for (int i = 0; i < MAX_PANES; i++)
      if (w->panes[i].doc) {
        assert(w->panes[i].cy == 0 && w->panes[i].rowoff == 0);
        assert(!w->panes[i].sel_active && !strcmp(buffer_line(&w->panes[i].doc->buf, 0), "fresh"));
      }
  }
  assert(buffer_char_count(&reload.document.buf) == 5);
  dispatch_key(&reload, 'Q');
  assert(file_write(argv[1], "external\n") == 0);
  documents_reload(&reload, root);
  assert(!strcmp(buffer_line(&reload.document.buf, 0), "freshQ") && reload.document.dirty);
  tabs_close(&reload);
  assert(reload.tab_count == 1 && reload.running && !reload.confirmation);
  tabs_close(&reload);
  assert(reload.confirmation && reload.document.views == 1 && reload.running);
  dispatch_key(&reload, 'n');
  assert(reload.tab_count == 1 && reload.running);
  tabs_close(&reload);
  dispatch_key(&reload, 'y');
  assert(!reload.running && !reload.confirmation);
  dispatch_shutdown(&reload);
  assert(!reload.document.buf.head && !reload.document.views);
  free(root);
  puts("Tab regressions passed.");
  return 0;
}
