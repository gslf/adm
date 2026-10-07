#include "layout.h"
#include "dispatch.h"
#include "undo.h"
#include "syntax.h"

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

// Compact rendering hides inactive panes. Reconstruct their spatial
// relationships from the tree and split ratios when no screen rectangles exist.
static void logical_areas(const workspace *w, int index, rect area, rect *areas) {
  const layout_node *n = &w->nodes[index];
  if (n->kind == LAYOUT_LEAF) {
    areas[n->pane] = area;
    return;
  }
  rect a = area, b = area;
  int length = n->kind == LAYOUT_VERTICAL ? area.width : area.height;
  int first = (int)((long long)length * n->ratio / RATIO_SCALE);
  if (n->kind == LAYOUT_VERTICAL) {
    a.width = first;
    b.x += first;
    b.width -= first;
  } else {
    a.height = first;
    b.y += first;
    b.height -= first;
  }
  logical_areas(w, n->first, a, areas);
  logical_areas(w, n->second, b, areas);
}

void layout_move(editor *e, int dx, int dy) {
  if (e->confirmation || e->new_file.active)
    return;
  layout_arrange(e);
  if (e->sidebar.focused) {
    if (dx > 0)
      e->sidebar.focused = 0;
    return;
  }
  rect areas[MAX_PANES] = {{0}};
  if (e->windows.compact)
    logical_areas(&e->windows, e->windows.root,
                  (rect){0, 0, 1000000, 1000000}, areas);
  else
    for (int i = 0; i < MAX_PANES; i++)
      areas[i] = e->windows.panes[i].area;
  rect from = areas[e->windows.active];
  int best = -1, best_overlap = -1, best_gap = 0, best_offset = 0;
  int panes[MAX_PANES], count = layout_order(e, panes);
  int left = from.x, top = from.y;
  int right = from.x + from.width, bottom = from.y + from.height;
  for (int i = 0; i < count; i++) {
    rect area = areas[panes[i]];
    if (area.x < left) left = area.x;
    if (area.y < top) top = area.y;
    if (area.x + area.width > right) right = area.x + area.width;
    if (area.y + area.height > bottom) bottom = area.y + area.height;
  }
  // If no pane lies in the requested direction, view the layout as a torus.
  // Cross-axis alignment still takes priority, preserving rows/columns in mixed
  // layouts. Including the current pane on this pass keeps a lone row/column
  // stationary instead of jumping diagonally to an unrelated pane.
  for (int wrap = 0; wrap < 2 && best < 0; wrap++) {
    if (wrap) {
      from.x -= dx * (right - left);
      from.y -= dy * (bottom - top);
    }
    for (int i = 0; i < count; i++) {
      int pane = panes[i];
      if (!wrap && pane == e->windows.active)
        continue;
      rect to = areas[pane];
      int gap = dx > 0 ? to.x - from.x - from.width :
                dx < 0 ? from.x - to.x - to.width :
                dy > 0 ? to.y - from.y - from.height : from.y - to.y - to.height;
      if (gap < 0)
        continue;
      int start = dx ? max(from.y, to.y) : max(from.x, to.x);
      int end = dx ? (from.y + from.height < to.y + to.height ?
                          from.y + from.height : to.y + to.height) :
                        (from.x + from.width < to.x + to.width ?
                          from.x + from.width : to.x + to.width);
      int overlap = end > start;
      int offset = dx ? (2 * to.y + to.height) - (2 * from.y + from.height) :
                        (2 * to.x + to.width) - (2 * from.x + from.width);
      if (offset < 0)
        offset = -offset;
      if (best < 0 || overlap > best_overlap ||
          (overlap == best_overlap && (gap < best_gap ||
            (gap == best_gap && offset < best_offset)))) {
        best = pane;
        best_overlap = overlap;
        best_gap = gap;
        best_offset = offset;
      }
    }
  }
  if (best >= 0) {
    e->windows.active = best;
    e->view = &e->windows.panes[best];
    layout_arrange(e);
  }
}

static void buffer_edited(buffer *b, const buffer_edit *edit, void *context) {
  editor *e = context;
  undo_note_edit(b, edit);
  document *doc = b->change_context;
  if (doc) { doc->change_id++; syntax_invalidate(doc, edit); }
  // Editing commands move the active cursor; the observer rebases other views.
  for (editor_tab *tab = e->tabs; tab; tab = tab->next) {
    workspace *w = tabs_workspace(e, tab);
    for (int i = 0; i < MAX_PANES; i++) {
      view *v = &w->panes[i];
      if (v->doc && &v->doc->buf == b && v != e->view)
        view_rebase(v, edit);
    }
  }
}

void layout_changed(editor *e) {
  for (editor_tab *tab = e->tabs; tab; tab = tab->next) {
    workspace *w = tabs_workspace(e, tab);
    for (int i = 0; i < MAX_PANES; i++)
      if (w->panes[i].doc)
        view_clamp(&w->panes[i]);
  }
}

static void reset_layout(editor *e, view initial) {
  memset(&e->windows, 0, sizeof e->windows);
  e->windows.count = 1;
  e->windows.nodes[0] = (layout_node){
    .used = 1, .parent = -1, .first = -1, .second = -1, .pane = 0
  };
  initial.used = 1;
  e->windows.panes[0] = initial;
  e->view = &e->windows.panes[0];
  layout_arrange(e);
}

void layout_bind_view(editor *e, view *v, document *doc) {
  if (!doc)
    return;
  int changed = v->doc != doc;
  view_bind_document(v, doc);
  if (changed)
    v->revision = ++e->next_view_revision;
  doc->buf.on_edit = buffer_edited;
  doc->buf.edit_context = e;
  undo_attach(doc);
}

void layout_set_document(editor *e, document *doc) {
  layout_bind_view(e, e->view, doc);
}

void layout_init(editor *e) {
  tabs_init(e);
  reset_layout(e, (view){0});
  if (e->document.filename || e->document.buf.head)
    layout_set_document(e, &e->document);
  else {
    e->view->revision = ++e->next_view_revision;
    sidebar_show(e, SIDEBAR_FILES);
  }
}

void layout_shutdown(editor *e) {
  tabs_shutdown(e);
  memset(&e->windows, 0, sizeof e->windows);
  e->view = NULL;
}

int layout_has_unsaved(const editor *e) {
  for (editor_tab *tab = e->tabs; tab; tab = tab->next) {
    workspace *w = tabs_workspace(e, tab);
    for (int i = 0; i < MAX_PANES; i++)
      if (w->panes[i].doc && w->panes[i].doc->dirty)
        return 1;
  }
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
  if (e->confirmation || e->new_file.active)
    return 0;
  layout_arrange(e);
  if (!layout_can_split(e, kind))
    return 0;
  workspace *w = &e->windows;
  int pane = 0, children[2], count = 0;
  while (w->panes[pane].used)
    pane++;
  for (int i = 0; i < MAX_LAYOUT_NODES && count < 2; i++)
    if (!w->nodes[i].used)
      children[count++] = i;
  int index = leaf(w, w->active);
  layout_node *n = &w->nodes[index];
  w->panes[pane] = (view){.used = 1, .revision = ++e->next_view_revision};
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
  w->active = pane;
  e->view = &w->panes[pane];
  sidebar_show(e, SIDEBAR_FILES);
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
  if (!e->view->doc)
    sidebar_show(e, SIDEBAR_FILES);
  else
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
  if (doc && doc->views == 1 && doc->dirty)
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
    int closing = 0;
    for (int j = 0; j < MAX_PANES; j++)
      closing += &e->windows.panes[j] != e->view && e->windows.panes[j].doc == doc;
    if (doc && doc->dirty && doc->views == closing) {
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
static int can_grow_width(const editor *e) {
  return e->sidebar.focused ? sidebar_can_resize(e, 2) :
         layout_can_resize(e, LAYOUT_VERTICAL, 2);
}
static int can_shrink_width(const editor *e) {
  return e->sidebar.focused ? sidebar_can_resize(e, -2) :
         layout_can_resize(e, LAYOUT_VERTICAL, -2);
}
static void split_horizontal(editor *e) { layout_split(e, LAYOUT_HORIZONTAL); }
static void split_vertical(editor *e) { layout_split(e, LAYOUT_VERTICAL); }
static void grow_height(editor *e) { layout_resize(e, LAYOUT_HORIZONTAL, 1); }
static void shrink_height(editor *e) { layout_resize(e, LAYOUT_HORIZONTAL, -1); }
static void grow_width(editor *e) {
  if (e->sidebar.focused) sidebar_resize(e, 2);
  else layout_resize(e, LAYOUT_VERTICAL, 2);
}
static void shrink_width(editor *e) {
  if (e->sidebar.focused) sidebar_resize(e, -2);
  else layout_resize(e, LAYOUT_VERTICAL, -2);
}

void layout_bindings(void) {
  dispatch_bind(CTRL('q'), split_horizontal);
  dispatch_bind(CTRL('u'), split_vertical);
  dispatch_bind_prefix_when('q', split_horizontal, "q", "Split above / below", can_horizontal);
  dispatch_bind_prefix_when('u', split_vertical, "u", "Split side by side", can_vertical);
  dispatch_bind_prefix_when(']', grow_height, "]", "Grow height", can_grow_height);
  dispatch_bind_prefix_when('[', shrink_height, "[", "Shrink height", can_shrink_height);
  dispatch_bind_prefix_global_when('}', grow_width, "}", "Grow width", can_grow_width);
  dispatch_bind_prefix_global_when('{', shrink_width, "{", "Shrink width", can_shrink_width);
  dispatch_bind_prefix_when('c', layout_close, "c", "Close current split", has_splits);
  dispatch_pair_prefix(']', '[', "Grow / Shrink height");
  dispatch_pair_prefix('}', '{', "Grow / Shrink width");
}
