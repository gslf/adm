#ifndef SIDEBAR_H
#define SIDEBAR_H

#include "view.h"

typedef enum { SIDEBAR_NONE, SIDEBAR_FILES, SIDEBAR_GIT } sidebar_kind;
typedef struct sidebar {
  sidebar_kind kind;
  sidebar_kind last;
  int focused;
} sidebar;

struct editor;
rect sidebar_area(const struct editor *e);
void sidebar_show(struct editor *e, sidebar_kind kind);
void sidebar_toggle(struct editor *e, sidebar_kind kind);
void sidebar_focus(struct editor *e);
void sidebar_key(struct editor *e, int key);
void sidebar_bindings(void);

#endif
