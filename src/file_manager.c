#include "file_manager.h"
#include "dispatch.h"
#include "path.h"
#include "search.h"

#include <stdio.h>
#include <stdlib.h>

void file_manager_init(editor *e) {
  e->files = (file_manager){.workspace_root = path_current_directory()};
}

void file_manager_shutdown(editor *e) {
  free(e->files.workspace_root);
  file_tree_free(&e->files.tree);
  e->files = (file_manager){0};
}

rect file_manager_area(const editor *e) {
  rect area = sidebar_area(e);
  if (e->sidebar.kind != SIDEBAR_FILES)
    area.width = 0;
  return area;
}

void file_manager_refresh(editor *e) {
  file_tree *tree = &e->files.tree;
  file_tree_free(tree);
  const char *root = e->files.workspace_root;
  if (root) {
    file_tree_init(tree, root);
  } else
    snprintf(tree->error, sizeof tree->error, "Cannot find current directory");
}

static void open_panel(editor *e) {
  sidebar_show(e, SIDEBAR_FILES);
}

static void opened(editor *e, int result) {
  if (result > 0)
    e->files.tree.error[0] = '\0';
  else if (result == 0)
    snprintf(e->files.tree.error, sizeof e->files.tree.error, "Cannot open file");
}

static void activate(editor *e) {
  file_tree *tree = &e->files.tree;
  const tree_entry *entry = file_tree_selected(tree);
  if (!entry)
    return;
  if (entry->directory) {
    if (entry->expanded)
      file_tree_collapse(tree);
    else
      file_tree_expand(tree);
    return;
  }
  documents_open(e, entry->path, opened);
}

void file_manager_key(editor *e, int key) {
  if (search_files_key(e, key))
    return;
  file_tree *tree = &e->files.tree;
  const tree_entry *entry = file_tree_selected(tree);
  int page = file_manager_area(e).height - 2;
  if (page < 1)
    page = 1;
  switch (key) {
  case KEY_UP: file_tree_move(tree, -1); break;
  case KEY_DOWN: file_tree_move(tree, 1); break;
  case KEY_PGUP: case META('v'): file_tree_move(tree, -page); break;
  case KEY_PGDOWN: case CTRL('v'): file_tree_move(tree, page); break;
  case KEY_HOME: case KEY_CTRL_HOME: case META('<'):
    file_tree_move(tree, -tree->count); break;
  case KEY_END: case KEY_CTRL_END: case META('>'):
    file_tree_move(tree, tree->count); break;
  case KEY_RIGHT: case CTRL('f'):
    if (entry && entry->directory) {
      if (!entry->expanded)
        file_tree_expand(tree);
      else if (tree->selected + 1 < tree->count &&
               tree->entries[tree->selected + 1].depth > entry->depth)
        file_tree_move(tree, 1);
    }
    break;
  case KEY_LEFT:
    if (entry && entry->expanded)
      file_tree_collapse(tree);
    else
      file_tree_parent(tree);
    break;
  case '\r': case '\n': activate(e); break;
  case CTRL('g'): case '\x1b': e->sidebar.focused = 0; break;
  default: break;
  }
}

void file_manager_bindings(void) {
  dispatch_bind_prefix_global('f', open_panel, "f", "Open File Explorer");
}
