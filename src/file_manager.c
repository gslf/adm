#include "file_manager.h"
#include "dispatch.h"
#include "path.h"
#include "search.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void file_manager_init(editor *e) {
  e->files = (file_manager){.workspace_root = path_current_directory()};
}

void file_manager_shutdown(editor *e) {
  free(e->files.workspace_root);
  free(e->files.delete_path);
  file_tree_free(&e->files.tree);
  e->files = (file_manager){0};
}

rect file_manager_area(const editor *e) {
  rect area = sidebar_area(e);
  if (e->sidebar.kind != SIDEBAR_FILES)
    area.width = 0;
  return area;
}

void file_manager_refresh_select(editor *e, const char *path) {
  file_tree *tree = &e->files.tree;
  if (tree->count) {
    file_tree_refresh(tree, path);
    return;
  }
  const char *root = e->files.workspace_root;
  if (root) {
    file_tree_init(tree, root);
  } else
    snprintf(tree->error, sizeof tree->error, "Cannot find current directory");
}

void file_manager_refresh(editor *e) {
  file_tree_free(&e->files.tree);
  file_manager_refresh_select(e, NULL);
}

static void cancel_delete(editor *e) {
  free(e->files.delete_path);
  e->files.delete_path = NULL;
}

static void delete_file(editor *e) {
  if (documents_delete_file(e, e->files.delete_path) < 0)
    snprintf(e->files.tree.error, sizeof e->files.tree.error,
             "Cannot delete file: %s", strerror(errno));
  else
    file_manager_refresh_select(e, NULL);
  cancel_delete(e);
}

static void begin_delete(editor *e, const tree_entry *entry) {
  if (!entry || entry->directory) {
    snprintf(e->files.tree.error, sizeof e->files.tree.error, "Select a file to delete");
    return;
  }
  if (git_panel_worktree_busy(e)) {
    snprintf(e->files.tree.error, sizeof e->files.tree.error, "Wait for the Git operation");
    return;
  }
  cancel_delete(e);
  e->files.delete_path = malloc(strlen(entry->path) + 1);
  if (!e->files.delete_path) {
    snprintf(e->files.tree.error, sizeof e->files.tree.error, "Out of memory");
    return;
  }
  strcpy(e->files.delete_path, entry->path);
  document *doc = documents_at_path(e, entry->path);
  if (!doc && errno) {
    snprintf(e->files.tree.error, sizeof e->files.tree.error, "Cannot inspect open buffers");
    cancel_delete(e);
    return;
  }
  snprintf(e->files.delete_prompt, sizeof e->files.delete_prompt,
           "Delete '%.160s'%s?", path_name(entry->path),
           doc && doc->dirty ? " and discard unsaved edits in all views" : "");
  dispatch_confirm_with_cancel(e, delete_file, cancel_delete, e->files.delete_prompt);
}

static void open_panel(editor *e) {
  sidebar_show(e, SIDEBAR_FILES);
}

static int selected_file(const editor *e) {
  const tree_entry *entry = file_tree_selected(&e->files.tree);
  return e->sidebar.focused && e->sidebar.kind == SIDEBAR_FILES &&
         !search_active(e) && !git_panel_worktree_busy(e) && entry && !entry->directory;
}

static void rename_selected(editor *e) {
  const tree_entry *entry = file_tree_selected(&e->files.tree);
  new_file_rename(e, entry->path);
}

static void delete_selected(editor *e) {
  begin_delete(e, file_tree_selected(&e->files.tree));
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
  dispatch_bind_prefix_global_when('n', rename_selected, "n", "Rename selected file", selected_file);
  dispatch_bind_prefix_global_when('d', delete_selected, "d", "Delete selected file", selected_file);
}
