#ifndef VIEW_H
#define VIEW_H

#include "document.h"

typedef struct rect {
  int x, y, width, height;
} rect;

// Each pane retains its document; shared text survives until its last view closes.
typedef struct view {
  document *doc;
  int cx, cy;
  int selx, sely, sel_active, sel_mode;
  int sticky;
  int rowoff, coloff;
  rect area;
} view;

void view_rebase(view *v, const buffer_edit *edit);
void view_clamp(view *v);
void view_bind_document(view *v, document *doc);
void view_dispose(view *v);

#endif
