#ifndef EDITOR_H
#define EDITOR_H

#include "layout.h"
#include "file_manager.h"

struct editor;
typedef void (*editor_action)(struct editor *e);

typedef struct editor {
  document document;
  workspace windows;
  view *view;
  file_manager files;
  int rows, cols;
  editor_action confirmation;
  editor_action confirmation_cancel;
  const char *confirmation_prompt;
  int prefix_active, prefix_scroll;
  int running;
} editor;

#endif
