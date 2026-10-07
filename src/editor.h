#ifndef EDITOR_H
#define EDITOR_H

#include "layout.h"
#include "file_manager.h"
#include "sidebar.h"
#include "git_panel.h"
#include "documents.h"
#include "tabs.h"
#include "new_file.h"

struct editor;
typedef void (*editor_action)(struct editor *e);

typedef struct editor {
  document document;
  workspace windows;
  editor_tab first_tab, *tabs, *active_tab;
  unsigned long next_tab_id;
  int tab_count;
  view *view;
  file_manager files;
  struct search_state *search;
  struct undo_state *undo;
  struct lsp_state *lsp;
  struct plugin_state *plugins;
  int syntax_enabled;
  sidebar sidebar;
  git_panel git;
  document_request open_request;
  new_file_prompt new_file;
  unsigned long next_view_revision;
  int rows, cols;
  char notice[256];
  editor_action confirmation;
  editor_action confirmation_cancel;
  const char *confirmation_prompt;
  int prefix_active, prefix_scroll;
  int move_active, help_active, help_scroll;
  int running;
} editor;

#endif
