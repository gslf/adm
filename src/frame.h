#ifndef FRAME_H
#define FRAME_H

#include "screen.h"

// The terminal keeps what was drawn, so a new frame only needs the cells that
// changed. Frames are drawn as cursor moves, SGR colours and text; they are
// read back into cells as a terminal would and compared with the screen.

// Encode into out what turns the shown frame into next, both drawn for a rows
// by cols terminal; shown is the frame the terminal displays. Returns 1 when
// out holds an update, 0 when the terminal already displays next, and -1 when
// next uses sequences the cache does not model and must be written as it is.
int frame_update(const abuf *shown, const abuf *next, int rows, int cols,
                 abuf *out);
// The frame last passed to frame_update as next is now on screen.
void frame_shown(void);
// The screen contents are unknown: the next update repaints every cell.
void frame_forget(void);

#endif
