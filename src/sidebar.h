#ifndef SIDEBAR_H
#define SIDEBAR_H

#include "view.h"

typedef enum { SIDEBAR_NONE, SIDEBAR_FILES, SIDEBAR_GIT } sidebar_kind;
typedef struct sidebar {
  sidebar_kind kind;
  sidebar_kind last;
  int focused;
  int width; // 0 chooses the default; a user-set width survives terminal resize.
} sidebar;

struct editor;
rect sidebar_area(const struct editor *e);
int sidebar_can_resize(const struct editor *e, int delta);
int sidebar_resize(struct editor *e, int delta);
void sidebar_show(struct editor *e, sidebar_kind kind);
void sidebar_toggle(struct editor *e, sidebar_kind kind);
void sidebar_focus(struct editor *e);
void sidebar_key(struct editor *e, int key);

#endif
