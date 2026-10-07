#include "shared_views.h"
#include "clipboard.h"
#include "command_menu.h"
#include "cursor.h"
#include "screen.h"
#include "search.h"
#include "utf8.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void type(editor *e, const char *text) {
  while (*text)
    dispatch_key(e, *text == '\n' ? (++text, '\r') : (unsigned char)*text++);
}

static void command_key(editor *e, int key) {
  test_prefix_shared(e, key);
}

static int listed(editor *e, int key) {
  int count = dispatch_prefix_count(e);
  for (int i = 0; i < count; i++)
    if (dispatch_prefix_at(e, i)->key == key)
      return 1;
  return 0;
}

static void sidebar_resizing(editor *e) {
  int cols = e->cols, rows = e->rows, ratios[MAX_LAYOUT_NODES];
  int active = e->windows.active;
  view *selected = e->view;
  for (int i = 0; i < MAX_LAYOUT_NODES; i++)
    ratios[i] = e->windows.nodes[i].ratio;
  e->cols = 120;
  e->rows = 32;
  for (sidebar_kind kind = SIDEBAR_FILES; kind <= SIDEBAR_GIT; kind++) {
    e->sidebar.width = 0;
    sidebar_show(e, kind);
    int width = sidebar_area(e).width;
    assert(e->sidebar.focused && listed(e, '}') && listed(e, '{'));
    command_key(e, '}');
    assert(sidebar_area(e).width == width + 2 && e->sidebar.focused);
    command_key(e, '{');
    assert(sidebar_area(e).width == width && e->sidebar.focused);
    while (listed(e, '}'))
      command_key(e, '}');
    assert(sidebar_area(e).width == e->cols - 13 && e->sidebar.focused);
    // A hidden command at the sidebar limit must not resize the last split.
    dispatch_key(e, CTRL('x'));
    dispatch_key(e, '}');
    assert(e->prefix_active && e->sidebar.focused);
    dispatch_key(e, '\x1b');
    int chosen = e->sidebar.width;
    sidebar_show(e, kind == SIDEBAR_FILES ? SIDEBAR_GIT : SIDEBAR_FILES);
    assert(sidebar_area(e).width == chosen && e->sidebar.focused);
    sidebar_show(e, kind);
    e->cols = 50;
    layout_arrange(e);
    assert(sidebar_area(e).width == 37 && e->sidebar.width == chosen);
    e->cols = 29;
    layout_arrange(e);
    assert(!sidebar_area(e).width && !e->sidebar.focused);
    e->cols = 120;
    sidebar_show(e, kind);
    assert(sidebar_area(e).width == chosen && e->sidebar.focused);
    sidebar_toggle(e, kind);
    assert(!sidebar_area(e).width);
    sidebar_show(e, kind);
    assert(sidebar_area(e).width == chosen);
    while (listed(e, '{'))
      command_key(e, '{');
    assert(sidebar_area(e).width == 10 && e->sidebar.focused);
    assert(listed(e, '}') && !listed(e, '{'));
    assert(e->windows.active == active && e->view == selected);
    for (int i = 0; i < MAX_LAYOUT_NODES; i++)
      assert(e->windows.nodes[i].ratio == ratios[i]);
  }
  e->sidebar.kind = SIDEBAR_NONE;
  e->sidebar.focused = e->sidebar.width = 0;
  e->cols = cols;
  e->rows = rows;
  layout_arrange(e);
}

static int validate_node(const workspace *w, int index, int parent, int *seen) {
  assert(index >= 0 && index < MAX_LAYOUT_NODES && !seen[index]);
  seen[index] = 1;
  const layout_node *n = &w->nodes[index];
  assert(n->used && n->parent == parent);
  if (n->kind == LAYOUT_LEAF) {
    assert(n->pane >= 0 && n->pane < MAX_PANES && w->panes[n->pane].doc);
    return 1;
  }
  int count = validate_node(w, n->first, index, seen) +
              validate_node(w, n->second, index, seen);
  if (!w->compact) {
    rect a = w->nodes[n->first].area, b = w->nodes[n->second].area;
    assert(a.x == n->area.x && a.y == n->area.y);
    if (n->kind == LAYOUT_VERTICAL) {
      assert(a.width + b.width + 1 == n->area.width);
      assert(b.x == a.x + a.width + 1 && b.y == a.y);
      assert(a.height == n->area.height && b.height == a.height);
    } else {
      assert(a.height + b.height + 1 == n->area.height);
      assert(b.y == a.y + a.height + 1 && b.x == a.x);
      assert(a.width == n->area.width && b.width == a.width);
    }
  }
  return count;
}

static void validate(editor *e) {
  workspace *w = &e->windows;
  int seen[MAX_LAYOUT_NODES] = {0};
  assert(validate_node(w, w->root, -1, seen) == w->count);
  int nodes = 0, panes = 0;
  for (int i = 0; i < MAX_LAYOUT_NODES; i++) {
    assert(w->nodes[i].used == seen[i]);
    nodes += seen[i];
  }
  for (int i = 0; i < MAX_PANES; i++) {
    if (!w->panes[i].doc)
      continue;
    document *doc = w->panes[i].doc;
    int references = 0;
    for (int j = 0; j < MAX_PANES; j++)
      references += w->panes[j].doc == doc;
    assert(doc->views == references);
    panes++;
    rect r = w->panes[i].area;
    assert(r.x >= 0 && r.y >= 0 && r.width >= 0 && r.height >= 0);
    if (r.width && r.height) {
      assert(r.x + r.width <= e->cols && r.y + r.height <= e->rows - 1);
      if (w->count > 1 && !w->compact)
        assert(r.width >= 12 && r.height >= 4);
    }
  }
  assert(panes == w->count && nodes == 2 * w->count - 1);
  assert(w->count >= 1 && w->count <= MAX_PANES);
  assert(e->view == &w->panes[w->active] && e->view->doc);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  editor e = {.document.filename = argv[1], .rows = 24, .cols = 80, .running = 1};
  buffer_init(&e.document.buf);
  dispatch_register(clipboard_module());
  dispatch_register(search_module());
  dispatch_init(&e);
  type(&e, "abc\ndef\nghi");
  dispatch_key(&e, META('<'));
  assert(listed(&e, 'q') && listed(&e, 'u'));
  const char *contextual = "][}{c";
  for (const char *key = contextual; *key; key++)
    assert(!listed(&e, *key));
  sidebar_resizing(&e); // Commands also work with just one editor split.
  command_key(&e, 'q');
  assert(e.windows.count == 2);
  validate(&e);
  assert(listed(&e, 'c') && listed(&e, ']') && !listed(&e, '}'));
  int original = e.windows.active;
  int height = e.view->area.height;
  command_key(&e, ']');
  assert(e.view->area.height == height + 1);
  command_key(&e, '[');
  assert(e.view->area.height == height);
  dispatch_key(&e, KEY_DOWN);
  dispatch_key(&e, CTRL('f'));
  assert(e.view->cy == 1 && e.view->cx == 1);
  layout_focus(&e, 1);
  assert(e.windows.active != original && e.view->cx == 0 && e.view->cy == 0);
  type(&e, "X\n");
  assert(e.windows.panes[original].cy == 2 && e.windows.panes[original].cx == 1);
  dispatch_key(&e, KEY_BACKSPACE);
  assert(e.windows.panes[original].cy == 1 && e.windows.panes[original].cx == 1);
  layout_focus(&e, -1);
  assert(e.windows.active == original && e.view->cy == 1 && e.view->cx == 1);
  command_key(&e, 'u');
  sidebar_resizing(&e); // Sidebar resizing preserves every split ratio.
  int width = e.view->area.width;
  command_key(&e, '}');
  assert(e.view->area.width == width + 2);
  command_key(&e, '{');
  assert(e.view->area.width == width);
  command_key(&e, 'u');
  assert(e.windows.count == MAX_PANES);
  assert(!test_shared_split(&e, LAYOUT_HORIZONTAL) && !test_shared_split(&e, LAYOUT_VERTICAL));
  assert(!listed(&e, 'q') && !listed(&e, 'u'));
  validate(&e);

  int panes[MAX_PANES], count = layout_order(&e, panes);
  for (int i = 0; i < count; i++) {
    assert(e.windows.active == panes[i]);
    layout_focus(&e, 1);
  }
  assert(e.windows.active == panes[0]);
  view *tracked = &e.windows.panes[panes[1]];
  tracked->cy = 1;
  tracked->cx = 2;
  tracked->sely = 2;
  tracked->selx = 3;
  tracked->sel_mode = tracked->sel_active = 1;
  dispatch_key(&e, META('<'));
  dispatch_key(&e, CTRL(' '));
  dispatch_key(&e, KEY_DOWN);
  dispatch_key(&e, KEY_DOWN);
  dispatch_key(&e, CTRL('f'));
  dispatch_key(&e, CTRL('d'));
  assert(e.document.buf.nlines == 1 && !strcmp(buffer_line(&e.document.buf, 0), "hi"));
  assert(tracked->cy == 0 && tracked->cx == 0);
  assert(tracked->sely == 0 && tracked->selx == 2);
  dispatch_key(&e, 0xe8);
  assert(!strcmp(buffer_line(&e.document.buf, 0), "\xc3\xa8hi"));
  assert(tracked->cx == 2 && tracked->selx == 4);
  layout_focus(&e, 1);
  assert(e.view == tracked && cursor_col(&e) == 1);
  dispatch_key(&e, CTRL('g'));

  e.cols = 100;
  e.rows = 32;
  layout_arrange(&e);
  validate(&e);
  e.cols = 15;
  e.rows = 8;
  layout_arrange(&e);
  assert(e.windows.compact && e.view->area.width == 15);
  int active = e.windows.active;
  layout_focus(&e, 1);
  assert(e.windows.active != active && e.view->area.width == 15);
  assert(!listed(&e, ']') && !listed(&e, '}'));
  e.cols = 80;
  e.rows = 24;
  layout_arrange(&e);
  assert(!e.windows.compact);
  validate(&e);

  e.rows = 12;
  layout_arrange(&e);
  dispatch_key(&e, CTRL('x'));
  command_menu_scroll(&e, 1000);
  assert(e.prefix_scroll > 0);
  abuf ab = {0};
  dispatch_draw(&e, &ab);
  ab_append(&ab, "\0", 1);
  assert(strstr(ab.b, "Save") && strstr(ab.b, "Up/Down scroll"));
  ab_free(&ab);
  dispatch_key(&e, CTRL('g'));
  layout_only(&e);
  assert(e.windows.count == 1 && e.view->cx >= 0);
  for (const char *key = contextual; *key; key++)
    assert(!listed(&e, *key));
  e.rows = 24;
  layout_arrange(&e);

  for (int i = 0; i < 500; i++) {
    test_shared_split(&e, i % 2 ? LAYOUT_HORIZONTAL : LAYOUT_VERTICAL);
    layout_focus(&e, 1);
    layout_resize(&e, i % 2 ? LAYOUT_VERTICAL : LAYOUT_HORIZONTAL, i % 3 - 1);
    if (i % 3 == 0)
      layout_close(&e);
    if (i % 7 == 0)
      layout_only(&e);
    validate(&e);
  }
  layout_only(&e);
  assert(test_shared_split(&e, LAYOUT_VERTICAL));
  while (layout_resize(&e, LAYOUT_VERTICAL, -1))
    validate(&e);
  assert(e.view->area.width == 12);
  assert(!layout_resize(&e, LAYOUT_VERTICAL, -1));
  layout_close(&e);
  assert(e.windows.count == 1 && e.document.dirty);
  dispatch_key(&e, META('>'));
  type(&e, "\nabcdefghijkl\nx\nabcdefghijkl");
  dispatch_key(&e, META('<'));
  dispatch_key(&e, KEY_DOWN);
  dispatch_key(&e, CTRL('e'));
  dispatch_key(&e, KEY_DOWN);
  assert(e.view->cy == 2 && e.view->cx == 1 && e.view->sticky == 12);
  assert(test_shared_split(&e, LAYOUT_VERTICAL));
  layout_focus(&e, 1);
  dispatch_key(&e, META('<'));
  type(&e, "!");
  layout_focus(&e, -1);
  assert(e.view->cx == 1 && e.view->sticky == 12);
  dispatch_key(&e, KEY_DOWN);
  assert(e.view->cx == 12);
  layout_only(&e);
  command_key(&e, CTRL('s'));
  assert(!e.document.dirty);
  dispatch_shutdown(&e);
  assert(e.document.views == 0 && !e.document.buf.head);
  puts("Split regressions passed.");
  return 0;
}
