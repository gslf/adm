#include "clipboard.h"
#include "dispatch.h"
#include "fileio.h"
#include "screen.h"
#include "search.h"
#include "utf8.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static void pause_tick(void) { Sleep(1); }
#else
#include <unistd.h>
static void pause_tick(void) { usleep(1000); }
#endif

static void settle(editor *e) {
  for (int i = 0; i < 20000; i++) {
    dispatch_tick(e);
    if (e->git.action == GIT_IDLE)
      return;
    pause_tick();
  }
  assert(!"Git operation did not finish");
}

static void prefix(editor *e, int key) {
  dispatch_key(e, CTRL('x'));
  assert(e->prefix_active);
  dispatch_key(e, key);
  assert(!e->prefix_active);
}

static void focus(editor *e) {
  if (e->sidebar.kind != SIDEBAR_GIT)
    prefix(e, 'v');
  if (!e->sidebar.focused)
    prefix(e, 'f');
  settle(e);
  assert(e->sidebar.kind == SIDEBAR_GIT && e->sidebar.focused);
}

static int find(const editor *e, const char *path, git_section section) {
  for (int i = 0; i < git_repository_items(&e->git.repo); i++) {
    git_item item = git_repository_item(&e->git.repo, i);
    if (item.section == section && !strcmp(item.file->path, path))
      return i;
  }
  return -1;
}

static void select_file(editor *e, const char *path, git_section section) {
  focus(e);
  int target = find(e, path, section);
  assert(target >= 0);
  while (e->git.selected != target)
    dispatch_key(e, e->git.selected < target ? CTRL('n') : CTRL('p'));
}

static void type(editor *e, const char *text) {
  for (int i = 0; text[i]; ) {
    int cp, count = utf8_decode(text + i, &cp);
    assert(count > 0);
    dispatch_key(e, cp);
    i += count;
  }
}

static void branch(editor *e, int merge, const char *name) {
  focus(e);
  dispatch_key(e, merge ? 'm' : 'b');
  settle(e);
  assert(e->git.mode == (merge ? GIT_PICK_MERGE : GIT_PICK_CHECKOUT));
  int target = -1;
  for (int i = 0; i < e->git.repo.branch_count; i++)
    if (!strcmp(e->git.repo.branches[i].name, name))
      target = i + !merge;
  assert(target >= 0);
  while (e->git.branch_selected != target)
    dispatch_key(e, e->git.branch_selected < target ? KEY_DOWN : KEY_UP);
  dispatch_key(e, '\r');
  settle(e);
}

static void test_parser(void) {
  git_repository repo = {0};
  const char status[] = "## main...origin/main [ahead 1]\0MM dual.txt\0"
                        "R  rename.txt\0old.txt\0?? space ' name.txt\0UU conflict.txt\0";
  assert(git_repository_status(&repo, status, sizeof status - 1) == 0);
  assert(repo.count == 4 && repo.staged == 2 && repo.unstaged == 2 && repo.conflicts == 1);
  assert(git_repository_items(&repo) == 5);
  git_item rename = git_repository_item(&repo, 2);
  assert(rename.section == GIT_STAGED && !strcmp(rename.file->original, "old.txt"));
  assert(git_repository_status(&repo, "## bad", 6) < 0 && repo.count == 4);
  const char branches[] = "refs/heads/main\nrefs/remotes/origin/main\nrefs/remotes/origin/HEAD\n";
  assert(git_repository_branches(&repo, branches, sizeof branches - 1) == 0);
  assert(repo.branch_count == 2 && repo.branches[1].remote);
  const char initial[] = "## No commits yet on main\0?? new.txt\0";
  assert(git_repository_status(&repo, initial, sizeof initial - 1) == 0 && repo.unborn);
  git_repository_free(&repo);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  test_parser();
  editor e = {.document.filename = argv[1], .rows = 28, .cols = 120, .running = 1};
  assert(load(&e.document.buf, e.document.filename) == 0);
  dispatch_register(clipboard_module());
  dispatch_register(search_module());
  dispatch_init(&e);
  assert(dispatch_prefix_find(&e, 'v') && dispatch_prefix_find(&e, 'f'));
  assert(!dispatch_prefix_find(&e, 'V'));
  prefix(&e, 't');
  view *target = e.view;
  prefix(&e, 'v');
  assert(e.sidebar.kind == SIDEBAR_GIT && !file_manager_area(&e).width && e.view == target);
  assert(e.sidebar.focused && e.windows.active == 0);
  settle(&e);
  assert(e.git.repo.root && e.git.repo.staged == 2 && e.git.repo.unstaged >= 5);
  assert(!e.git.failed && !strncmp(e.git.repo.head, "main", 4));
  prefix(&e, 'f');
  assert(e.sidebar.kind == SIDEBAR_GIT && !e.sidebar.focused && e.view == target);
  prefix(&e, 'f');
  settle(&e);
  assert(e.sidebar.kind == SIDEBAR_GIT && e.sidebar.focused && e.view == target);
  prefix(&e, '3');
  prefix(&e, 'o');
  select_file(&e, "dual.txt", GIT_STAGED);
  int pane = e.windows.active;
  dispatch_key(&e, '\r');
  assert(e.git.action == GIT_DIFF && e.windows.count == 2 && !e.view->doc->readonly);
  prefix(&e, 'o'); // The diff retains the pane selected when the job was launched.
  settle(&e);
  assert(e.windows.active == pane && e.view->doc->readonly && !e.sidebar.focused);
  int lines = e.view->doc->buf.nlines;
  dispatch_key(&e, 'X');
  dispatch_key(&e, CTRL('d'));
  dispatch_key(&e, CTRL('h'));
  dispatch_key(&e, CTRL('y'));
  dispatch_key(&e, CTRL(' '));
  dispatch_key(&e, CTRL('n'));
  dispatch_key(&e, CTRL('w'));
  assert(!e.view->doc->dirty && e.view->doc->buf.nlines == lines);
  prefix(&e, CTRL('s'));
  assert(e.view->doc->readonly && !e.view->doc->filename);
  prefix(&e, 'o');
  dispatch_key(&e, 'Q');
  document *dirty = e.view->doc;
  select_file(&e, "dual.txt", GIT_UNSTAGED);
  dispatch_key(&e, '\r');
  settle(&e);
  assert(e.confirmation && e.open_request.text && dirty->dirty);
  dispatch_key(&e, 'n');
  assert(!e.confirmation && !e.open_request.text && e.view->doc == dirty);
  prefix(&e, CTRL('s'));
  settle(&e);
  select_file(&e, "literal[1].txt", GIT_UNSTAGED);
  dispatch_key(&e, 's');
  settle(&e);
  assert(find(&e, "literal[1].txt", GIT_STAGED) >= 0 && find(&e, "new file.txt", GIT_UNSTAGED) >= 0);
  select_file(&e, "literal[1].txt", GIT_STAGED);
  dispatch_key(&e, 'u');
  settle(&e);
  assert(find(&e, "literal[1].txt", GIT_UNSTAGED) >= 0);
  select_file(&e, "renamed file.txt", GIT_STAGED);
  dispatch_key(&e, 'u');
  settle(&e);
  assert(find(&e, "renamed file.txt", GIT_UNSTAGED) >= 0);
  select_file(&e, "new file.txt", GIT_UNSTAGED);
  dispatch_key(&e, '\r');
  settle(&e);
  assert(e.view->doc->readonly);
  int added = 0;
  for (int i = 0; i < e.view->doc->buf.nlines; i++)
    added |= !strcmp(buffer_line(&e.view->doc->buf, i), "+new content");
  assert(added);

  focus(&e);
  dispatch_key(&e, 'U');
  settle(&e);
  assert(!e.git.repo.staged && e.git.repo.unstaged > 0);
  dispatch_key(&e, 'S');
  settle(&e);
  assert(e.git.repo.staged > 0 && !e.git.repo.unstaged);
  dispatch_key(&e, 'c');
  assert(e.git.mode == GIT_MESSAGE);
  dispatch_key(&e, '\r');
  assert(e.git.mode == GIT_MESSAGE && e.git.action == GIT_IDLE);
  type(&e, "Add tests $(touch should-not-exist) 'quotes' è");
  dispatch_key(&e, CTRL('a'));
  dispatch_key(&e, 'X');
  dispatch_key(&e, CTRL('h'));
  dispatch_key(&e, CTRL('e'));
  assert(strstr(e.git.input, "è"));
  dispatch_key(&e, '\r');
  settle(&e);
  assert(!e.git.repo.staged && !e.git.repo.unstaged && !e.git.failed && e.git.mode == GIT_FILES);
  assert(!file_read("should-not-exist"));

  // Open alpha from the tree, then verify checkout reloads its existing document.
  prefix(&e, 't');
  if (!e.sidebar.focused)
    prefix(&e, 'f');
  int index = -1;
  for (int i = 0; i < e.files.tree.count; i++)
    if (strstr(e.files.tree.entries[i].path, "/alpha.txt"))
      index = i;
  assert(index >= 0 && e.sidebar.kind == SIDEBAR_FILES && e.windows.count == 2);
  e.files.tree.selected = index;
  dispatch_key(&e, '\r');
  document *alpha = e.view->doc;
  assert(!alpha->readonly);
  dispatch_key(&e, '!');
  focus(&e);
  dispatch_key(&e, 'b');
  assert(e.git.mode == GIT_FILES && e.git.failed && e.git.action == GIT_IDLE);
  prefix(&e, CTRL('s'));
  settle(&e);
  branch(&e, 0, "feature");
  assert(e.git.failed && !strncmp(e.git.repo.head, "main", 4));
  select_file(&e, "alpha.txt", GIT_UNSTAGED);
  dispatch_key(&e, 's');
  settle(&e);
  dispatch_key(&e, 'c');
  type(&e, "Save buffer change");
  dispatch_key(&e, '\r');
  settle(&e);
  prefix(&e, 'o');
  documents_open(&e, "vanishing.txt", NULL);
  document *vanishing = e.view->doc;
  assert(!vanishing->readonly);
  prefix(&e, 'o');
  branch(&e, 0, "feature");
  assert(!e.git.failed && !strncmp(e.git.repo.head, "feature", 7));
  assert(alpha == e.view->doc && !strcmp(buffer_line(&alpha->buf, 0), "feature"));
  assert(vanishing->buf.nlines == 0 && vanishing->filename);
  branch(&e, 1, "main");
  assert(e.git.repo.conflicts && e.git.failed && !alpha->dirty);
  assert(strstr(buffer_line(&alpha->buf, 0), "<<<<<<<"));
  focus(&e);
  dispatch_key(&e, 'c');
  assert(e.git.mode == GIT_FILES && e.git.failed);
  select_file(&e, "alpha.txt", GIT_CONFLICT);
  dispatch_key(&e, 'o');
  assert(!e.sidebar.focused && e.view->doc == alpha);
  dispatch_key(&e, META('<'));
  dispatch_key(&e, CTRL(' '));
  dispatch_key(&e, META('>'));
  dispatch_key(&e, CTRL('d'));
  type(&e, "resolved");
  prefix(&e, CTRL('s'));
  settle(&e);
  select_file(&e, "alpha.txt", GIT_CONFLICT);
  dispatch_key(&e, 's');
  settle(&e);
  assert(!e.git.repo.conflicts && e.git.repo.staged);
  dispatch_key(&e, 'c');
  type(&e, "Resolve merge");
  dispatch_key(&e, '\r');
  settle(&e);
  assert(!e.git.failed && !e.git.repo.count);
  dispatch_key(&e, 'P');
  assert(e.git.action == GIT_PUSH);
  settle(&e);
  assert(!e.git.failed);
  branch(&e, 0, "pull-target");
  assert(!e.git.failed && strstr(e.git.repo.head, "behind 1"));
  dispatch_key(&e, 'p');
  settle(&e);
  assert(!e.git.failed && !strstr(e.git.repo.head, "behind"));
  branch(&e, 0, "origin/pull-target");
  assert(!e.git.failed && !strncmp(e.git.repo.head, "pull-target", 11));
  char *remote = file_read("remote-target.txt");
  assert(remote && !strcmp(remote, "remote content\n"));
  free(remote);

  // A new branch is a basic checkout action; no shell parses its name.
  dispatch_key(&e, 'b');
  settle(&e);
  assert(e.git.branch_selected == 0);
  dispatch_key(&e, '\r');
  assert(e.git.mode == GIT_NEW_BRANCH);
  type(&e, "panel-branch");
  dispatch_key(&e, '\r');
  settle(&e);
  assert(!e.git.failed && !strncmp(e.git.repo.head, "panel-branch", 12));
  target = e.view;
  int active = e.windows.active;
  prefix(&e, 'v');
  assert(e.sidebar.kind == SIDEBAR_NONE && e.sidebar.last == SIDEBAR_GIT);
  assert(!e.sidebar.focused);
  prefix(&e, 'v');
  settle(&e);
  assert(e.sidebar.kind == SIDEBAR_GIT && e.sidebar.focused);
  assert(e.view == target && e.windows.active == active && e.windows.count == 2);
  prefix(&e, 'v');
  prefix(&e, 'f');
  settle(&e);
  assert(e.sidebar.kind == SIDEBAR_GIT && e.sidebar.focused);
  assert(e.view == target && e.windows.active == active && e.windows.count == 2);
  prefix(&e, 't');
  assert(e.sidebar.kind == SIDEBAR_FILES && e.sidebar.focused);
  prefix(&e, 't');
  assert(e.sidebar.kind == SIDEBAR_NONE && e.sidebar.last == SIDEBAR_FILES);
  prefix(&e, 'f');
  assert(e.sidebar.kind == SIDEBAR_FILES && e.sidebar.focused && e.view == target);
  prefix(&e, 'v');
  settle(&e);
  assert(e.sidebar.kind == SIDEBAR_GIT && e.sidebar.focused);
  e.cols = 20;
  layout_arrange(&e);
  assert(!sidebar_area(&e).width && !e.sidebar.focused && e.windows.count == 2);
  e.cols = 120;
  layout_arrange(&e);
  assert(sidebar_area(&e).width && e.sidebar.kind == SIDEBAR_GIT);
  dispatch_shutdown(&e);
  assert(!e.git.repo.files && !e.git.repo.root && !e.git.process.running && !e.view);
  return 0;
}
