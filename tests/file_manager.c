#include "clipboard.h"
#include "file_manager.h"
#include "path.h"
#include "search.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void prefix(editor *e, int key) {
  dispatch_key(e, CTRL('x'));
  assert(e->prefix_active);
  dispatch_key(e, key);
  assert(!e->prefix_active);
}

static void select_entry(editor *e, const char *name) {
  if (!e->sidebar.focused)
    prefix(e, 'f');
  file_tree *tree = &e->files.tree;
  int target = -1;
  for (int i = 0; i < tree->count; i++)
    if (!strcmp(path_name(tree->entries[i].path), name)) {
      target = i;
      break;
    }
  assert(target >= 0 && e->sidebar.focused);
  while (tree->selected != target)
    dispatch_key(e, tree->selected < target ? CTRL('n') : CTRL('p'));
}

static void open_entry(editor *e, const char *name) {
  select_entry(e, name);
  dispatch_key(e, '\r');
}

int main(int argc, char **argv) {
  assert(argc == 2);
  editor e = {.document.filename = argv[1], .rows = 24, .cols = 80, .running = 1};
  assert(load(&e.document.buf, e.document.filename) == 0);
  dispatch_register(clipboard_module());
  dispatch_register(search_module());
  dispatch_init(&e);
  view *first = e.view;
  assert(e.sidebar.kind != SIDEBAR_FILES && !e.sidebar.focused && e.windows.count == 1);
  assert(dispatch_prefix_find(&e, 't') && dispatch_prefix_find(&e, 'f'));
  assert(!dispatch_prefix_find(&e, '^') && !dispatch_prefix_find(&e, '-'));
  prefix(&e, 'f');
  assert(e.sidebar.kind == SIDEBAR_FILES && e.sidebar.focused && e.view == first);
  assert(e.sidebar.last == SIDEBAR_FILES && e.windows.active == 0);
  prefix(&e, 'f');
  assert(e.sidebar.kind == SIDEBAR_FILES && !e.sidebar.focused && e.view == first);
  assert(e.view->area.x == file_manager_area(&e).width + 1);
  file_tree *tree = &e.files.tree;
  assert(tree->entries[0].directory && tree->entries[0].expanded);
  assert(!strcmp(path_name(tree->entries[1].path), "empty"));
  assert(!strcmp(path_name(tree->entries[2].path), "folder"));
  assert(tree->entries[1].directory && tree->entries[2].directory);

  int original_count = tree->count;
  select_entry(&e, "folder");
  assert(e.view == first && e.windows.active == 0);
  dispatch_key(&e, 'X');
  dispatch_key(&e, CTRL('s'));
  assert(!strcmp(buffer_line(&e.document.buf, 0), "alpha"));
  dispatch_key(&e, KEY_RIGHT);
  assert(tree->count == original_count + 1 && file_tree_selected(tree)->expanded);
  dispatch_key(&e, KEY_RIGHT);
  assert(!strcmp(path_name(file_tree_selected(tree)->path), "nested.txt"));
  dispatch_key(&e, KEY_LEFT);
  assert(!strcmp(path_name(file_tree_selected(tree)->path), "folder"));
  dispatch_key(&e, KEY_LEFT);
  assert(tree->count == original_count && !file_tree_selected(tree)->expanded);
  select_entry(&e, "empty");
  dispatch_key(&e, '\r');
  assert(file_tree_selected(tree)->expanded && tree->count == original_count);
  dispatch_key(&e, '\r');
  assert(!file_tree_selected(tree)->expanded);
  dispatch_key(&e, KEY_END);
  file_tree_scroll(tree, 3);
  assert(tree->offset > 0 && tree->selected == tree->count - 1);
  dispatch_key(&e, META('<'));
  file_tree_scroll(tree, 3);
  assert(tree->selected == 0 && tree->offset == 0);
  prefix(&e, 'f');
  assert(!e.sidebar.focused && e.sidebar.kind == SIDEBAR_FILES && e.view == first);

  // File-manager focus is independent of the pane targeted by open and split commands.
  prefix(&e, '3');
  prefix(&e, 'o');
  view *target = e.view;
  int active = e.windows.active;
  open_entry(&e, "beta.txt");
  assert(!e.sidebar.focused && e.view == target && e.windows.active == active);
  assert(e.document.views == 1 && target->doc != &e.document);
  document *beta = target->doc;
  assert(beta->views == 1 && !strcmp(buffer_line(&beta->buf, 0), "beta"));
  dispatch_key(&e, '!');
  assert(beta->dirty);
  prefix(&e, 'o');
  open_entry(&e, "beta.txt");
  assert(!e.confirmation && e.view == first && e.view->doc == beta && beta->views == 2);
  assert(e.document.views == 0 && !e.document.buf.head);
  assert(!strcmp(buffer_line(&beta->buf, 0), "!beta"));

  // Replacing a view releases only its reference; another view retains unsaved edits.
  open_entry(&e, "alpha.txt");
  assert(!e.confirmation && beta->views == 1 && e.view == first);
  document *alpha = first->doc;
  prefix(&e, 'o');
#ifndef _WIN32
  open_entry(&e, "beta.link");
  assert(e.view->doc == beta && !e.confirmation && beta->views == 1);
#endif
  open_entry(&e, "gamma.txt");
  assert(e.confirmation && e.open_request.path && e.view->doc == beta);
  dispatch_key(&e, KEY_NONE);
  assert(e.confirmation);
  dispatch_key(&e, 'n');
  assert(!e.confirmation && !e.open_request.path && e.view->doc == beta);
  assert(e.sidebar.focused && beta->dirty && beta->views == 1);
  open_entry(&e, "gamma.txt");
  dispatch_key(&e, 'y');
  assert(!e.confirmation && !e.open_request.path && !e.sidebar.focused);
  document *gamma = target->doc;
  assert(gamma != alpha && gamma->views == 1);
  assert(!strcmp(buffer_line(&gamma->buf, 0), "gamma"));

  open_entry(&e, "nul.bin");
  assert(e.view->doc == gamma && !e.confirmation && e.sidebar.focused && tree->error[0]);
  assert(!e.open_request.path);
#ifndef _WIN32
  open_entry(&e, "pipe");
  assert(e.view->doc == gamma && tree->error[0]); // Non-regular files cannot block the UI.
#endif
  open_entry(&e, "gone.txt");
  assert(e.view->doc != gamma && !e.sidebar.focused);
  document *gone = e.view->doc;
  assert(remove("gone.txt") == 0);
  open_entry(&e, "gone.txt");
  assert(e.view->doc == gone && tree->error[0] && e.sidebar.focused);
  dispatch_key(&e, CTRL('g'));
  assert(!e.sidebar.focused);

  select_entry(&e, "folder");
  dispatch_key(&e, KEY_RIGHT);
  open_entry(&e, "nested.txt");
  assert(!strcmp(buffer_line(&e.view->doc->buf, 0), "nested"));
  prefix(&e, 'f');
  assert(e.sidebar.focused);
  prefix(&e, 'g');
  assert(!e.sidebar.focused); // Editor commands return focus before starting a prompt.
  dispatch_key(&e, '1');
  dispatch_key(&e, '\r');
  dispatch_key(&e, '!');
  document *nested = e.view->doc;
  assert(nested->dirty && !strcmp(buffer_line(&nested->buf, 0), "!nested"));

  prefix(&e, '2');
  prefix(&e, 'o');
  prefix(&e, 'o');
  prefix(&e, '2');
  assert(e.windows.count == MAX_PANES && nested->views == 2);
  dispatch_key(&e, '@');
  assert(alpha->dirty && nested->dirty);
  prefix(&e, 'f');
  assert(e.sidebar.focused && e.windows.count == MAX_PANES);
  int selected_pane = e.windows.active;
  prefix(&e, 't');
  assert(e.sidebar.kind != SIDEBAR_FILES && !e.sidebar.focused && e.windows.active == selected_pane);
  assert(e.sidebar.last == SIDEBAR_FILES);
  prefix(&e, 'f');
  assert(e.sidebar.kind == SIDEBAR_FILES && e.sidebar.focused && e.windows.active == selected_pane);
  prefix(&e, 'o');
  assert(!e.sidebar.focused && e.windows.active != selected_pane);
  prefix(&e, 'f');
  e.cols = 20;
  layout_arrange(&e);
  assert(!file_manager_area(&e).width && !e.sidebar.focused && e.windows.count == MAX_PANES);
  e.cols = 80;
  layout_arrange(&e);
  assert(file_manager_area(&e).width && e.sidebar.kind == SIDEBAR_FILES);

  // Exit inspects every distinct visible document, including an inactive dirty one.
  prefix(&e, CTRL('c'));
  assert(e.confirmation && e.running);
  dispatch_key(&e, 'n');
  assert(e.running && !e.confirmation && nested->dirty);
  prefix(&e, CTRL('c'));
  dispatch_key(&e, 'y');
  assert(!e.running);
  dispatch_shutdown(&e);
  assert(!e.open_request.path && !e.files.tree.entries && !e.view);
  return 0;
}
