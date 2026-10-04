#include "file_manager.h"
#include "dispatch.h"
#include "path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void file_manager_init(editor *e) {
  e->files = (file_manager){0};
}

void file_manager_shutdown(editor *e) {
  free(e->files.pending_path);
  file_tree_free(&e->files.tree);
  e->files = (file_manager){0};
}

rect file_manager_area(const editor *e) {
  int width = 0;
  if (e->files.visible && e->cols >= 30 && e->rows >= 5) {
    width = e->cols / 3;
    if (width > 30)
      width = 30;
  }
  return (rect){0, 1, width, e->rows > 2 ? e->rows - 2 : 0};
}

static void show(editor *e) {
  file_tree *tree = &e->files.tree;
  if (!tree->count) {
    char *root = path_current_directory();
    if (root) {
      file_tree_init(tree, root);
      free(root);
    } else
      snprintf(tree->error, sizeof tree->error, "Cannot find current directory");
  }
  e->files.visible = 1;
  layout_arrange(e);
}

static void toggle(editor *e) {
  if (e->files.visible) {
    e->files.visible = e->files.focused = 0;
    layout_arrange(e);
  } else
    show(e);
}

static void focus(editor *e) {
  if (!e->files.visible)
    show(e);
  e->files.focused = file_manager_area(e).width > 0 && !e->files.focused;
}

// Pane references are the document registry, so reopening a file shares its edits.
static document *visible_document(const editor *e, const char *path) {
  for (int i = 0; i < MAX_PANES; i++) {
    document *doc = e->windows.panes[i].doc;
    if (document_matches(doc, path))
      return doc;
  }
  return NULL;
}

static void cancel_open(editor *e) {
  free(e->files.pending_path);
  e->files.pending_path = NULL;
}

static void open_now(editor *e) {
  const char *path = e->files.pending_path;
  document *doc = visible_document(e, path);
  if (!doc)
    doc = document_open(path);
  if (doc) {
    layout_set_document(e, doc);
    e->files.focused = 0;
    e->files.tree.error[0] = '\0';
  } else
    snprintf(e->files.tree.error, sizeof e->files.tree.error, "Cannot open file");
  cancel_open(e);
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
  char *path = malloc(strlen(entry->path) + 1);
  if (!path) {
    snprintf(tree->error, sizeof tree->error, "Out of memory");
    return;
  }
  strcpy(path, entry->path);
  e->files.pending_path = path;
  document *current = e->view->doc;
  if (current->dirty && current->views == 1 && !document_matches(current, path))
    dispatch_confirm_with_cancel(e, open_now, cancel_open, "Replace unsaved buffer?");
  else
    open_now(e);
}

void file_manager_key(editor *e, int key) {
  file_tree *tree = &e->files.tree;
  const tree_entry *entry = file_tree_selected(tree);
  int page = file_manager_area(e).height - 2;
  if (page < 1)
    page = 1;
  switch (key) {
  case KEY_UP: case CTRL('p'): file_tree_move(tree, -1); break;
  case KEY_DOWN: case CTRL('n'): file_tree_move(tree, 1); break;
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
  case KEY_LEFT: case CTRL('b'):
    if (entry && entry->expanded)
      file_tree_collapse(tree);
    else
      file_tree_parent(tree);
    break;
  case '\r': case '\n': activate(e); break;
  case CTRL('g'): case '\x1b': e->files.focused = 0; break;
  default: break;
  }
}

void file_manager_bindings(void) {
  dispatch_bind_prefix_global('t', toggle, "t", "Toggle file manager");
  dispatch_bind_prefix_global('f', focus, "f", "Focus file manager / editor");
}
