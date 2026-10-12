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
  // With word wrap, wrapoff rows of line rowoff sit above the area and sticky
  // is a column inside the cursor's row; coloff only scrolls unwrapped text.
  int rowoff, coloff, wrapoff;
  rect area;
} view;

void view_rebase(view *v, const buffer_edit *edit);
void view_clamp(view *v);
void view_bind_document(view *v, document *doc);
void view_dispose(view *v);

#endif
