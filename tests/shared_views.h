#ifndef TEST_SHARED_VIEWS_H
#define TEST_SHARED_VIEWS_H

#include "dispatch.h"
#include <assert.h>

// Existing ownership/rebasing suites build shared views explicitly. Production
// tab/split commands now create empty views; empty_views.c tests that UI policy.
static void test_bind_shared(editor *e, view source) {
  assert(!e->view->doc && e->view->used);
  layout_set_document(e, source.doc);
  unsigned long revision = e->view->revision;
  rect area = e->view->area;
  *e->view = source;
  e->view->revision = revision;
  e->view->area = area;
  e->view->sel_active = e->view->sel_mode = 0;
}

static inline int test_shared_split(editor *e, layout_kind kind) {
  view source = *e->view;
  sidebar panel = e->sidebar;
  int active = e->windows.active;
  if (!layout_split(e, kind))
    return 0;
  test_bind_shared(e, source);
  e->windows.active = active;
  e->view = &e->windows.panes[active];
  e->sidebar = panel;
  layout_arrange(e);
  return 1;
}

static inline int test_shared_tab(editor *e) {
  view source = *e->view;
  sidebar panel = e->sidebar;
  if (!tabs_new(e))
    return 0;
  test_bind_shared(e, source);
  e->sidebar = panel;
  layout_arrange(e);
  return 1;
}

static inline void test_prefix_shared(editor *e, int key) {
  view source = *e->view;
  sidebar panel = e->sidebar;
  int active = e->windows.active, count = e->windows.count;
  dispatch_key(e, CTRL('x'));
  dispatch_key(e, key);
  assert(!e->prefix_active);
  if ((key == 'q' || key == 'u') && e->windows.count > count) {
    test_bind_shared(e, source);
    e->windows.active = active;
    e->view = &e->windows.panes[active];
    e->sidebar = panel;
    e->sidebar.focused = 0;
    layout_arrange(e);
  } else if (key == 't') {
    test_bind_shared(e, source);
    e->sidebar = panel;
    e->sidebar.focused = 0;
    layout_arrange(e);
  }
}

#endif
