#ifndef CURSOR_H
#define CURSOR_H

#include "dispatch.h"

// Cursor movement in file coordinates.
void cursor_up(editor *e);
void cursor_down(editor *e);
void cursor_left(editor *e);
void cursor_right(editor *e);

// Update the scroll offsets so the cursor stays visible.
void cursor_scroll(editor *e);

#endif
