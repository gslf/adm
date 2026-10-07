#include "sidebar.h"
#include "dispatch.h"
#include "editor.h"
#include "git_panel.h"

#define MIN_SIDEBAR_WIDTH 10
#define MIN_EDITOR_WIDTH 12

static int clamp_width(const editor *e, int width) {
  int maximum = e->cols - MIN_EDITOR_WIDTH - 1;
  return width < MIN_SIDEBAR_WIDTH ? MIN_SIDEBAR_WIDTH :
         width > maximum ? maximum : width;
}

rect sidebar_area(const editor *e) {
  int width = 0;
  int minimum_rows = e->sidebar.kind == SIDEBAR_GIT ? 9 : 5;
  if (e->sidebar.kind != SIDEBAR_NONE && e->cols >= 30 && e->rows >= minimum_rows) {
    width = e->cols / 3;
    int maximum = e->sidebar.kind == SIDEBAR_GIT ? 40 : 30;
    if (width > maximum)
      width = maximum;
    if (e->sidebar.width)
      width = clamp_width(e, e->sidebar.width);
  }
  return (rect){0, 1, width, e->rows > 2 ? e->rows - 2 : 0};
}

static int resized_width(const editor *e, int delta) {
  int current = sidebar_area(e).width;
  return current ? clamp_width(e, current + delta) : 0;
}

int sidebar_can_resize(const editor *e, int delta) {
  int current = sidebar_area(e).width;
  return current > 0 && resized_width(e, delta) != current;
}

int sidebar_resize(editor *e, int delta) {
  if (!sidebar_can_resize(e, delta))
    return 0;
  e->sidebar.width = resized_width(e, delta);
  layout_arrange(e);
  return 1;
}

void sidebar_show(editor *e, sidebar_kind kind) {
  e->sidebar.kind = kind;
  if (kind != SIDEBAR_NONE)
    e->sidebar.last = kind;
  if (kind == SIDEBAR_FILES && !e->files.tree.count)
    file_manager_refresh(e);
  else if (kind == SIDEBAR_GIT)
    git_panel_refresh(e);
  layout_arrange(e);
  e->sidebar.focused = kind != SIDEBAR_NONE && sidebar_area(e).width > 0;
}

void sidebar_toggle(editor *e, sidebar_kind kind) {
  if (e->sidebar.kind == kind) {
    e->sidebar.kind = SIDEBAR_NONE;
    e->sidebar.focused = 0;
    layout_arrange(e);
  } else
    sidebar_show(e, kind);
}

void sidebar_focus(editor *e) {
  // Focus belongs to the sidebar; closing it preserves the last chosen panel.
  if (e->sidebar.kind == SIDEBAR_NONE) {
    sidebar_show(e, e->sidebar.last ? e->sidebar.last : SIDEBAR_FILES);
    return;
  } else if (e->sidebar.kind == SIDEBAR_GIT && !e->sidebar.focused)
    git_panel_refresh(e);
  e->sidebar.focused = sidebar_area(e).width > 0 && !e->sidebar.focused;
}

void sidebar_key(editor *e, int key) {
  if (e->sidebar.kind == SIDEBAR_FILES)
    file_manager_key(e, key);
  else if (e->sidebar.kind == SIDEBAR_GIT)
    git_panel_key(e, key);
}
