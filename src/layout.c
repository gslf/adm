#include "layout.h"
#include "dispatch.h"

#include <string.h>

#define MIN_WIDTH 12
#define MIN_HEIGHT 4
#define RATIO_SCALE 10000

static int max(int a, int b) { return a > b ? a : b; }
static int clamp(int n, int low, int high) {
  return n < low ? low : n > high ? high : n;
}

static int leaf(const workspace *w, int pane) {
  for (int i = 0; i < MAX_LAYOUT_NODES; i++)
    if (w->nodes[i].used && w->nodes[i].kind == LAYOUT_LEAF &&
        w->nodes[i].pane == pane)
      return i;
  return -1;
}

static int minimum(const workspace *w, int index, layout_kind axis) {
  const layout_node *n = &w->nodes[index];
  if (n->kind == LAYOUT_LEAF)
    return axis == LAYOUT_VERTICAL ? MIN_WIDTH : MIN_HEIGHT;
  int a = minimum(w, n->first, axis);
  int b = minimum(w, n->second, axis);
  return n->kind == axis ? a + b + 1 : max(a, b);
}

static int extent(rect area, layout_kind axis) {
  return axis == LAYOUT_VERTICAL ? area.width : area.height;
}

static void arrange_node(workspace *w, int index, rect area) {
  layout_node *n = &w->nodes[index];
  n->area = area;
  if (n->kind == LAYOUT_LEAF) {
    w->panes[n->pane].area = area;
    return;
  }
  int available = extent(area, n->kind) - 1;
  int first = clamp(available * n->ratio / RATIO_SCALE,
                    minimum(w, n->first, n->kind),
                    available - minimum(w, n->second, n->kind));
  rect a = area, b = area;
  if (n->kind == LAYOUT_VERTICAL) {
    a.width = first;
    b.x += first + 1;
    b.width -= first + 1;
  } else {
    a.height = first;
    b.y += first + 1;
    b.height -= first + 1;
  }
  arrange_node(w, n->first, a);
  arrange_node(w, n->second, b);
}

void layout_arrange(editor *e) {
  workspace *w = &e->windows;
  rect area = {0, 1, max(e->cols, 0), max(e->rows - 2, 0)};
  rect sidebar = sidebar_area(e);
  if (sidebar.width == 0)
    e->sidebar.focused = 0;
  if (sidebar.width > 0) {
    area.x = sidebar.width + 1;
    area.width -= area.x;
  }
  for (int i = 0; i < MAX_PANES; i++)
    w->panes[i].area = (rect){0};
  w->compact = w->count > 1 &&
      (area.width < minimum(w, w->root, LAYOUT_VERTICAL) ||
       area.height < minimum(w, w->root, LAYOUT_HORIZONTAL));
  // Preserve the tree on small terminals; focus can still reach every pane.
  if (w->compact)
    e->view->area = area;
  else
    arrange_node(w, w->root, area);
}

rect layout_content(const editor *e, const view *v) {
  rect area = v->area;
  if (e->windows.count > 1 && area.height > 0) {
    area.y++;
    area.height--;
  }
  return area;
}

static void collect(const workspace *w, int node, int *panes, int *count) {
  const layout_node *n = &w->nodes[node];
  if (n->kind == LAYOUT_LEAF)
    panes[(*count)++] = n->pane;
  else {
    collect(w, n->first, panes, count);
    collect(w, n->second, panes, count);
  }
}

int layout_order(const editor *e, int panes[MAX_PANES]) {
  int count = 0;
  collect(&e->windows, e->windows.root, panes, &count);
  return count;
}

static void buffer_edited(buffer *b, const buffer_edit *edit, void *context) {
  editor *e = context;
  // Editing commands move the active cursor; the observer rebases other views.
  for (int i = 0; i < MAX_PANES; i++) {
    view *v = &e->windows.panes[i];
    if (!v->doc || &v->doc->buf != b || v == e->view)
      continue;
    view_rebase(v, edit);
  }
}

void layout_changed(editor *e) {
  for (int i = 0; i < MAX_PANES; i++)
    if (e->windows.panes[i].doc)
      view_clamp(&e->windows.panes[i]);
}

static void reset_layout(editor *e, view initial) {
  memset(&e->windows, 0, sizeof e->windows);
  e->windows.count = 1;
  e->windows.nodes[0] = (layout_node){
    .used = 1, .parent = -1, .first = -1, .second = -1, .pane = 0
  };
  e->windows.panes[0] = initial;
  e->view = &e->windows.panes[0];
  layout_arrange(e);
}

void layout_set_document(editor *e, document *doc) {
  if (!doc)
    return;
  int changed = e->view->doc != doc;
  view_bind_document(e->view, doc);
  if (changed)
    e->view->revision = ++e->next_view_revision;
  doc->buf.on_edit = buffer_edited;
  doc->buf.edit_context = e;
}

void layout_init(editor *e) {
  reset_layout(e, (view){0});
  layout_set_document(e, &e->document);
}

void layout_shutdown(editor *e) {
  for (int i = 0; i < MAX_PANES; i++)
    view_dispose(&e->windows.panes[i]);
  memset(&e->windows, 0, sizeof e->windows);
  e->view = NULL;
}

int layout_has_unsaved(const editor *e) {
  for (int i = 0; i < MAX_PANES; i++)
    if (e->windows.panes[i].doc && e->windows.panes[i].doc->dirty)
      return 1;
  return 0;
}

int layout_can_split(const editor *e, layout_kind kind) {
  if (kind != LAYOUT_HORIZONTAL && kind != LAYOUT_VERTICAL)
    return 0;
  return e->windows.count < MAX_PANES && !e->windows.compact &&
         extent(e->view->area, kind) >=
             2 * (kind == LAYOUT_VERTICAL ? MIN_WIDTH : MIN_HEIGHT) + 1 &&
         e->view->area.width >= MIN_WIDTH && e->view->area.height >= MIN_HEIGHT;
}

int layout_split(editor *e, layout_kind kind) {
  layout_arrange(e);
  if (!layout_can_split(e, kind))
    return 0;
  workspace *w = &e->windows;
  int pane = 0, children[2], count = 0;
  while (w->panes[pane].doc)
    pane++;
  for (int i = 0; i < MAX_LAYOUT_NODES && count < 2; i++)
    if (!w->nodes[i].used)
      children[count++] = i;
  int index = leaf(w, w->active);
  layout_node *n = &w->nodes[index];
  w->panes[pane] = *e->view;
  w->panes[pane].revision = ++e->next_view_revision;
  document_retain(w->panes[pane].doc);
  w->panes[pane].sel_active = w->panes[pane].sel_mode = 0;
  for (int i = 0; i < 2; i++)
    w->nodes[children[i]] = (layout_node){
      .used = 1, .parent = index, .first = -1, .second = -1,
      .pane = i == 0 ? w->active : pane
    };
  n->kind = kind;
  n->first = children[0];
  n->second = children[1];
  n->ratio = RATIO_SCALE / 2;
  w->count++;
  layout_arrange(e);
  return 1;
}

void layout_focus(editor *e, int step) {
  int panes[MAX_PANES], count = layout_order(e, panes), index = 0;
  while (panes[index] != e->windows.active)
    index++;
  index = (index + step % count + count) % count;
  e->windows.active = panes[index];
  e->view = &e->windows.panes[panes[index]];
  layout_arrange(e);
}

// Resize the nearest ancestor on the requested axis, preserving nested splits.
static int resize_target(const editor *e, layout_kind kind, int delta,
                         int *new_ratio) {
  const workspace *w = &e->windows;
  if (w->count < 2 || w->compact)
    return -1;
  int child = leaf(w, w->active);
  int parent = w->nodes[child].parent;
  while (parent >= 0 && w->nodes[parent].kind != kind) {
    child = parent;
    parent = w->nodes[parent].parent;
  }
  if (parent < 0)
    return -1;
  const layout_node *n = &w->nodes[parent];
  int available = extent(n->area, kind) - 1;
  int current = extent(w->nodes[n->first].area, kind);
  int wanted = current + (child == n->first ? delta : -delta);
  if (wanted < minimum(w, n->first, kind) ||
      wanted > available - minimum(w, n->second, kind))
    return -1;
  *new_ratio = (wanted * RATIO_SCALE + available - 1) / available;
  return parent;
}

int layout_can_resize(const editor *e, layout_kind kind, int delta) {
  int ratio;
  return resize_target(e, kind, delta, &ratio) >= 0;
}

int layout_resize(editor *e, layout_kind kind, int delta) {
  layout_arrange(e);
  int ratio, node = resize_target(e, kind, delta, &ratio);
  if (node < 0)
    return 0;
  e->windows.nodes[node].ratio = ratio;
  layout_arrange(e);
  return 1;
}

static int first_pane(const workspace *w, int node) {
  while (w->nodes[node].kind != LAYOUT_LEAF)
    node = w->nodes[node].first;
  return w->nodes[node].pane;
}

static void close_now(editor *e) {
  workspace *w = &e->windows;
  if (w->count < 2)
    return;
  int node = leaf(w, w->active), parent = w->nodes[node].parent;
  int sibling = w->nodes[parent].first == node ? w->nodes[parent].second
                                             : w->nodes[parent].first;
  int grandparent = w->nodes[parent].parent;
  if (grandparent < 0)
    w->root = sibling;
  else if (w->nodes[grandparent].first == parent)
    w->nodes[grandparent].first = sibling;
  else
    w->nodes[grandparent].second = sibling;
  w->nodes[sibling].parent = grandparent;
  w->nodes[node].used = w->nodes[parent].used = 0;
  view_dispose(&w->panes[w->active]);
  w->active = first_pane(w, sibling);
  e->view = &w->panes[w->active];
  w->count--;
  layout_arrange(e);
}

void layout_close(editor *e) {
  if (e->windows.count < 2)
    return;
  const document *doc = e->view->doc;
  if (doc->views == 1 && doc->dirty)
    dispatch_confirm(e, close_now, "Close without saving?");
  else
    close_now(e);
}

static void only_now(editor *e) {
  view saved = *e->view;
  for (int i = 0; i < MAX_PANES; i++)
    if (&e->windows.panes[i] != e->view)
      view_dispose(&e->windows.panes[i]);
  reset_layout(e, saved);
}

void layout_only(editor *e) {
  for (int i = 0; i < MAX_PANES; i++) {
    const document *doc = e->windows.panes[i].doc;
    if (doc && doc != e->view->doc && doc->dirty) {
      dispatch_confirm(e, only_now, "Close other buffers?");
      return;
    }
  }
  only_now(e);
}

static int has_splits(const editor *e) { return e->windows.count > 1; }
static int can_horizontal(const editor *e) { return layout_can_split(e, LAYOUT_HORIZONTAL); }
static int can_vertical(const editor *e) { return layout_can_split(e, LAYOUT_VERTICAL); }
static int can_grow_height(const editor *e) { return layout_can_resize(e, LAYOUT_HORIZONTAL, 1); }
static int can_shrink_height(const editor *e) { return layout_can_resize(e, LAYOUT_HORIZONTAL, -1); }
static int can_grow_width(const editor *e) { return layout_can_resize(e, LAYOUT_VERTICAL, 2); }
static int can_shrink_width(const editor *e) { return layout_can_resize(e, LAYOUT_VERTICAL, -2); }
static void split_horizontal(editor *e) { layout_split(e, LAYOUT_HORIZONTAL); }
static void split_vertical(editor *e) { layout_split(e, LAYOUT_VERTICAL); }
static void next_pane(editor *e) { layout_focus(e, 1); }
static void previous_pane(editor *e) { layout_focus(e, -1); }
static void grow_height(editor *e) { layout_resize(e, LAYOUT_HORIZONTAL, 1); }
static void shrink_height(editor *e) { layout_resize(e, LAYOUT_HORIZONTAL, -1); }
static void grow_width(editor *e) { layout_resize(e, LAYOUT_VERTICAL, 2); }
static void shrink_width(editor *e) { layout_resize(e, LAYOUT_VERTICAL, -2); }

void layout_bindings(void) {
  dispatch_bind_prefix_when('2', split_horizontal, "2", "Split above / below", can_horizontal);
  dispatch_bind_prefix_when('3', split_vertical, "3", "Split side by side", can_vertical);
  dispatch_bind_prefix_when('o', next_pane, "o", "Next pane", has_splits);
  dispatch_bind_prefix_when('O', previous_pane, "O", "Previous pane", has_splits);
  dispatch_bind_prefix_when(']', grow_height, "]", "Grow height", can_grow_height);
  dispatch_bind_prefix_when('[', shrink_height, "[", "Shrink height", can_shrink_height);
  dispatch_bind_prefix_when('}', grow_width, "}", "Grow width", can_grow_width);
  dispatch_bind_prefix_when('{', shrink_width, "{", "Shrink width", can_shrink_width);
  dispatch_bind_prefix_when('0', layout_close, "0", "Close current pane", has_splits);
  dispatch_bind_prefix_when('1', layout_only, "1", "Keep only this pane", has_splits);
  dispatch_pair_prefix('o', 'O', "Next / Previous pane");
  dispatch_pair_prefix(']', '[', "Grow / Shrink height");
  dispatch_pair_prefix('}', '{', "Grow / Shrink width");
}
