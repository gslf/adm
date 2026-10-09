#ifndef SEARCH_H
#define SEARCH_H
#include "dispatch.h"
module *search_module(void);
void search_tick(editor *e);
int search_active(const editor *e);
void search_cancel(editor *e);
int search_cursor(const editor *e, int *x, int *y);
int search_files_draw(editor *e, struct abuf *ab, rect area);
int search_files_key(editor *e, int key);
// Test/diagnostic access; counts are final only when done is true.
void search_status(editor *e, unsigned long long *matches, int *done);
#endif
