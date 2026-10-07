#ifndef TABS_H
#define TABS_H

#include "layout.h"

typedef struct editor_tab {
  struct editor_tab *next, *previous;
  unsigned long id;
  // The active workspace lives in editor.windows; inactive ones live here.
  workspace windows;
} editor_tab;

struct editor;
void tabs_init(struct editor *e);
void tabs_shutdown(struct editor *e);
workspace *tabs_workspace(const struct editor *e, const editor_tab *tab);
editor_tab *tabs_find(const struct editor *e, unsigned long id);
int tabs_new(struct editor *e);
void tabs_focus(struct editor *e, int step);
void tabs_close(struct editor *e);
void tabs_bindings(void);
struct abuf;
void tabs_draw(const struct editor *e, struct abuf *ab, int width);

struct editor_tab *tabs_prepare_document(struct document *doc);
void tabs_commit_document(struct editor *e, struct editor_tab *tab);
void tabs_discard_document(struct editor_tab *tab);

#endif
