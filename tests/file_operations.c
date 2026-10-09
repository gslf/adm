#include "shared_views.h"
#include "fileio.h"
#include "path.h"
#include "screen.h"
#include "syntax.h"
#include "utf8.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static void select_file(editor *e, const char *name) {
  sidebar_show(e, SIDEBAR_FILES);
  for (int i = 0; i < e->files.tree.count; i++)
    if (!strcmp(path_name(e->files.tree.entries[i].path), name)) {
      e->files.tree.selected = i;
      return;
    }
  assert(!"Missing fixture file");
}

static void name(editor *e, const char *text) {
  assert(e->new_file.active && e->new_file.rename_path);
  dispatch_key(e, KEY_HOME);
  while (e->new_file.length)
    dispatch_key(e, KEY_DELETE);
  while (*text) {
    int cp;
    text += utf8_decode(text, &cp);
    dispatch_key(e, cp);
  }
}

static void rename_file(editor *e, const char *source, const char *target) {
  select_file(e, source);
  test_prefix_shared(e, 'n');
  name(e, target);
  dispatch_key(e, '\r');
}

static int exists(const char *path) {
  struct stat info;
  return stat(path, &info) == 0;
}

static void explorer_commands(editor *e) {
  select_file(e, "alpha.txt");
  const command_binding *rename = dispatch_prefix_find(e, 'n');
  const command_binding *delete = dispatch_prefix_find(e, 'd');
  assert(rename && delete && rename->preserve_focus && delete->preserve_focus);
  assert(!strcmp(rename->label, "Rename selected file"));
  assert(!strcmp(delete->label, "Delete selected file"));
  char direct[32];
  dispatch_direct_bindings(rename->cmd, direct, sizeof direct);
  assert(!direct[0]);
  dispatch_direct_bindings(delete->cmd, direct, sizeof direct);
  assert(!direct[0]);
  const int keys[] = {'r', 'd', KEY_DELETE};
  for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
    dispatch_key(e, keys[i]);
    assert(!e->new_file.active && !e->confirmation && !e->files.delete_path);
    assert(exists("alpha.txt"));
  }
  int rows = e->rows;
  e->rows = 80;
  dispatch_key(e, CTRL('x'));
  abuf menu = {0};
  dispatch_draw(e, &menu);
  ab_append(&menu, "", 1);
  assert(strstr(menu.b, "Rename selected file") && strstr(menu.b, "Delete selected file"));
  ab_free(&menu);
  dispatch_key(e, '\x1b');
  e->rows = rows;
  e->sidebar.focused = 0;
  assert(!dispatch_prefix_find(e, 'n') && !dispatch_prefix_find(e, 'd'));
  e->sidebar.focused = 1;
  e->sidebar.kind = SIDEBAR_GIT;
  assert(!dispatch_prefix_find(e, 'n') && !dispatch_prefix_find(e, 'd'));
  e->sidebar.kind = SIDEBAR_FILES;
}

static void contents(const char *path, const char *text) {
  char *actual = file_read(path);
  if (!actual || strcmp(actual, text))
    fprintf(stderr, "%s: expected <%s>, got <%s>\n", path, text, actual ? actual : "missing");
  assert(actual && !strcmp(actual, text));
  free(actual);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  editor e = {.document.filename = argv[1], .rows = 24, .cols = 100, .running = 1};
  assert(!load(&e.document.buf, argv[1]));
  dispatch_init(&e);
  explorer_commands(&e);
  e.sidebar.focused = 0;
  document *doc = e.view->doc;
  dispatch_key(&e, '!');
  assert(doc->dirty && !strcmp(buffer_line(&doc->buf, 0), "!alpha"));
#ifdef _WIN32
  // Git and the explorer may receive long and 8.3 spellings of the same path.
  char short_file[MAX_PATH], short_root[MAX_PATH];
  assert(GetShortPathNameA(argv[1], short_file, sizeof short_file));
  assert(GetShortPathNameA(e.files.workspace_root, short_root, sizeof short_root));
  const char *filename = doc->filename;
  doc->filename = short_file;
  assert(documents_unsaved_in(&e, e.files.workspace_root));
  assert(documents_unsaved_in(&e, short_root));
  doc->filename = filename;
  char *pending = path_join(short_root, "not-created.txt");
  char *resolved = path_absolute(pending);
  char *expected = path_join(e.files.workspace_root, "not-created.txt");
  assert(resolved && !strcmp(resolved, expected));
  free(pending);
  free(resolved);
  free(expected);
#endif
  assert(tabs_new(&e));
  documents_open(&e, argv[1], NULL);
  test_prefix_shared(&e, 'u');
  documents_open(&e, argv[1], NULL);
  assert(doc->views == 3 && e.tab_count == 2 && e.windows.count == 2);
  syntax_prepare(doc, 1);
  void *history = doc->history;
  view first = e.first_tab.windows.panes[0];
  view second = e.windows.panes[0], third = e.windows.panes[1];
  rename_file(&e, "alpha.txt", "renamed file.c");
  assert(!e.new_file.active && e.sidebar.focused);
  assert(!exists(argv[1]) && exists("renamed file.c"));
  assert(!strcmp(path_name(doc->filename), "renamed file.c"));
  assert(doc->dirty && doc->history == history && doc->views == 3);
  assert(!memcmp(&first, &e.first_tab.windows.panes[0], sizeof first));
  assert(!memcmp(&second, &e.windows.panes[0], sizeof second));
  assert(!memcmp(&third, &e.windows.panes[1], sizeof third));
  contents("renamed file.c", "alpha\n");
  assert(documents_find(&e, doc->filename) == doc);
  unsigned long long generation = doc->change_id;
  rename_file(&e, "renamed file.c", "renamed file.c");
  assert(!e.new_file.active && doc->change_id == generation);
  test_prefix_shared(&e, CTRL('s'));
  assert(!doc->dirty && !exists(argv[1]));
  contents("renamed file.c", "!alpha\n");
  dispatch_key(&e, CTRL('z'));
  assert(doc->dirty && !strcmp(buffer_line(&doc->buf, 0), "alpha"));
  dispatch_key(&e, META('z'));
  assert(!doc->dirty && !strcmp(buffer_line(&doc->buf, 0), "!alpha"));

  // Existing destinations and invalid names must leave both files untouched.
  rename_file(&e, "renamed file.c", "beta.txt");
  assert(e.new_file.active && e.new_file.error[0]);
  contents("beta.txt", "beta\n");
  contents("renamed file.c", "!alpha\n");
  name(&e, "folder/new.txt");
  dispatch_key(&e, '\r');
  assert(e.new_file.active && e.new_file.error[0]);
  dispatch_key(&e, '\x1b');
  assert(!e.new_file.active && e.sidebar.focused);

  // Case-only renames must work on Windows as well as case-sensitive Unix.
  rename_file(&e, "renamed file.c", "Renamed file.c");
  assert(!e.new_file.active && !strcmp(path_name(doc->filename), "Renamed file.c"));
  assert(!strcmp(path_name(file_tree_selected(&e.files.tree)->path), "Renamed file.c"));

  // A buffer for a not-yet-created destination must not be silently replaced.
  char *reserved = path_absolute("reserved.txt");
  document target = {.filename = reserved};
  buffer_init(&target.buf);
  layout_set_document(&e, &target);
  rename_file(&e, "Renamed file.c", "reserved.txt");
  assert(e.new_file.active && e.new_file.error[0] && !exists("reserved.txt"));
  dispatch_key(&e, '\x1b');
  layout_set_document(&e, doc);
  free(reserved);

  // Nested operations retain expanded folders and select the renamed file.
  select_file(&e, "folder");
  dispatch_key(&e, KEY_RIGHT);
  rename_file(&e, "nested.txt", "nested renamed.txt");
  assert(!e.new_file.active && exists("folder/nested renamed.txt"));
  assert(!strcmp(path_name(file_tree_selected(&e.files.tree)->path), "nested renamed.txt"));
  select_file(&e, "folder");
  assert(file_tree_selected(&e.files.tree)->expanded);
  dispatch_key(&e, KEY_DELETE);
  assert(!e.confirmation && exists("folder"));
  dispatch_key(&e, 'r');
  assert(!e.new_file.active);
  assert(!dispatch_prefix_find(&e, 'n') && !dispatch_prefix_find(&e, 'd'));

  // Delete snapshots the selected path, cancels safely, and confirms unsaved loss.
  select_file(&e, "Renamed file.c");
  e.sidebar.focused = 0;
  dispatch_key(&e, '?');
  assert(doc->dirty);
  select_file(&e, "Renamed file.c");
  test_prefix_shared(&e, 'd');
  assert(e.confirmation && strstr(e.confirmation_prompt, "unsaved"));
  dispatch_key(&e, 'n');
  assert(!e.confirmation && !e.files.delete_path && exists("Renamed file.c"));
  assert(doc->views == 3 && doc->dirty);
  test_prefix_shared(&e, 'd');
  assert(e.confirmation);
  // A failed deletion leaves shared buffers intact.
  assert(!remove("Renamed file.c"));
  dispatch_key(&e, 'y');
  assert(!e.confirmation && e.files.tree.error[0] && doc->views == 3);
  assert(!file_write("Renamed file.c", "disk\n"));
  test_prefix_shared(&e, 'd');
  dispatch_key(&e, 'y');
  assert(!exists("Renamed file.c") && !e.files.delete_path);
  assert(e.tab_count == 2 && e.windows.count == 2 && e.sidebar.focused);
  assert(!e.first_tab.windows.panes[0].doc && !e.windows.panes[0].doc && !e.windows.panes[1].doc);
  e.sidebar.focused = 0;
  test_prefix_shared(&e, CTRL('s'));
  assert(!exists("Renamed file.c"));
  select_file(&e, "beta.txt");
  test_prefix_shared(&e, 'd');
  dispatch_key(&e, '\x1b');
  assert(exists("beta.txt") && !e.files.delete_path);
  test_prefix_shared(&e, 'd');
  dispatch_key(&e, 'y');
  assert(!exists("beta.txt"));

#ifndef _WIN32
  // Renaming/deleting an alias must not retarget or close the canonical buffer.
  assert(!file_write("alias-target.txt", "keep\n"));
  assert(!symlink("alias-target.txt", "alias.link"));
  file_manager_refresh(&e);
  select_file(&e, "alias-target.txt");
  dispatch_key(&e, '\r');
  document *canonical = e.view->doc;
  rename_file(&e, "alias.link", "new alias.link");
  struct stat info;
  assert(!lstat("new alias.link", &info) && S_ISLNK(info.st_mode));
  assert(e.view->doc == canonical && !strcmp(path_name(canonical->filename), "alias-target.txt"));
  test_prefix_shared(&e, 'd');
  dispatch_key(&e, 'y');
  assert(e.view->doc == canonical && exists("alias-target.txt"));
  contents("alias-target.txt", "keep\n");
#endif
  dispatch_shutdown(&e);
  return 0;
}
