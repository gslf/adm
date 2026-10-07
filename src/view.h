#ifndef VIEW_H
#define VIEW_H

#include "document.h"

typedef struct rect {
  int x, y, width, height;
} rect;

// A used pane can be empty. Attached documents survive until their last view closes.
typedef struct view {
  int used;
  unsigned long revision;
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
