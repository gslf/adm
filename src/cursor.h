#ifndef CURSOR_H
#define CURSOR_H

#include "dispatch.h"

// Cursor movement in file coordinates. Drops the selection.
void cursor_up(editor *e);
void cursor_down(editor *e);
void cursor_left(editor *e);
void cursor_right(editor *e);

// Movement by whole words. Drops the selection.
void cursor_word_left(editor *e);
void cursor_word_right(editor *e);

// Same movement, extending the selection from the anchor.
void cursor_select_up(editor *e);
void cursor_select_down(editor *e);
void cursor_select_left(editor *e);
void cursor_select_right(editor *e);

// Drop the selection.
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
