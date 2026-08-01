#ifndef CURSOR_H
#define CURSOR_H

#include "dispatch.h"

// Cursor movement in file coordinates. In selection mode it extends the
// selection from the anchor, otherwise it drops the selection.
void cursor_up(editor *e);
void cursor_down(editor *e);
void cursor_left(editor *e);
void cursor_right(editor *e);

// Movement by whole words.
void cursor_word_left(editor *e);
void cursor_word_right(editor *e);

// One screenful up or down, and to the start of the line.
void cursor_page_up(editor *e);
void cursor_page_down(editor *e);
void cursor_home(editor *e);

// Turn selection mode on, anchored at the cursor, or off again.
void cursor_select_toggle(editor *e);

// Leave selection mode and drop the selection.
void selection_clear(editor *e);

// Remove the selected text and leave the cursor where it started.
void selection_delete(editor *e);

// Selection bounds sorted in document order, end column exclusive.
// Returns 0 and leaves the outputs untouched if nothing is selected.
int selection_range(const editor *e, int *sr, int *sc, int *er, int *ec);

// Screen column of the cursor, derived from its byte offset.
int cursor_col(const editor *e);

// Take the current column as the target for later vertical movement. Call it
// after any edit that moves the cursor sideways.
void cursor_mark_column(editor *e);

// Update the scroll offsets so the cursor stays visible.
void cursor_scroll(editor *e);

#endif
