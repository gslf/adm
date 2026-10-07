#ifndef LAYOUT_H
#define LAYOUT_H

#include "view.h"

#define MAX_PANES 4
#define MAX_LAYOUT_NODES (2 * MAX_PANES - 1)

typedef enum { LAYOUT_LEAF, LAYOUT_HORIZONTAL, LAYOUT_VERTICAL } layout_kind;

typedef struct layout_node {
  int used, parent, first, second, pane;
  layout_kind kind;
  int ratio;
  rect area;
} layout_node;

typedef struct workspace {
  // A bounded binary tree supports mixed splits without allocation or grids.
  view panes[MAX_PANES];
  layout_node nodes[MAX_LAYOUT_NODES];
  int root, active, count, compact;
} workspace;

struct editor;

void layout_init(struct editor *e);
void layout_shutdown(struct editor *e);
// Callers confirm first when replacing a dirty document's last view.
void layout_set_document(struct editor *e, document *doc);
void layout_bind_view(struct editor *e, view *v, document *doc);
int layout_has_unsaved(const struct editor *e);
void layout_arrange(struct editor *e);
rect layout_content(const struct editor *e, const view *v);
int layout_order(const struct editor *e, int panes[MAX_PANES]);
int layout_can_split(const struct editor *e, layout_kind kind);
int layout_split(struct editor *e, layout_kind kind);
void layout_focus(struct editor *e, int step);
void layout_move(struct editor *e, int dx, int dy);
int layout_can_resize(const struct editor *e, layout_kind kind, int delta);
int layout_resize(struct editor *e, layout_kind kind, int delta);
void layout_close(struct editor *e);
void layout_only(struct editor *e);
void layout_changed(struct editor *e);
void layout_bindings(void);

#endif
