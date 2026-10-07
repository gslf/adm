#ifndef NAVIGATION_H
#define NAVIGATION_H

struct editor;
struct abuf;
void navigation_bindings(void);
int navigation_modal_key(struct editor *e, int key);
int navigation_quick_key(int key);
void navigation_draw(struct editor *e, struct abuf *ab);

#endif
