#ifndef SCREEN_H
#define SCREEN_H

#include "dispatch.h"

// Append buffer: accumulates all output and writes it in a single write().
typedef struct abuf {
  char *b;
  int len;
} abuf;

void ab_append(abuf *ab, const char *s, int len);
void ab_free(abuf *ab);

// Width of the line-number column (number plus a space).
int screen_gutter(const editor *e);

// Redraw the whole screen (status bars, text, cursor).
void screen_refresh(editor *e);

// Clear the screen (used on exit).
void screen_clear(void);

#endif
