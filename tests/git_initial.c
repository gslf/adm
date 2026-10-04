#include "dispatch.h"
#include "fileio.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
static void pause_tick(void) { Sleep(1); }
#define CHANGE_DIRECTORY _chdir
#else
#include <unistd.h>
static void pause_tick(void) { usleep(1000); }
#define CHANGE_DIRECTORY chdir
#endif

static void settle(editor *e) {
  for (int i = 0; i < 10000; i++) {
    dispatch_tick(e);
    if (e->git.action == GIT_IDLE)
      return;
    pause_tick();
  }
  assert(!"Git operation did not finish");
}

static void prefix(editor *e, int key) {
  dispatch_key(e, CTRL('x'));
  dispatch_key(e, key);
  assert(!e->prefix_active);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  editor e = {.document.filename = argv[1], .rows = 24, .cols = 100, .running = 1};
  assert(load(&e.document.buf, e.document.filename) == 0);
  dispatch_init(&e);
  prefix(&e, 'v');
  assert(e.sidebar.kind == SIDEBAR_GIT && e.sidebar.focused);
  settle(&e);
  assert(e.git.repo.unborn && e.git.repo.unstaged == 2 && !e.git.repo.staged);
  dispatch_key(&e, 's');
  settle(&e);
  assert(e.git.repo.staged == 1 && e.git.repo.unstaged == 1);
  dispatch_key(&e, 'u');
  settle(&e);
  assert(!e.git.failed && !e.git.repo.staged && e.git.repo.unstaged == 2);
  dispatch_key(&e, 'S');
  settle(&e);
  assert(e.git.repo.staged == 2 && !e.git.repo.unstaged);
  dispatch_key(&e, 'U');
  settle(&e);
  assert(!e.git.failed && !e.git.repo.staged && e.git.repo.unstaged == 2);
  char *file = file_read("alpha.txt");
  assert(file && !strcmp(file, "new content\n"));
  free(file);
  dispatch_key(&e, 'S');
  settle(&e);
  dispatch_key(&e, '\r');
  settle(&e);
  assert(e.view->doc->readonly && !e.confirmation);
  prefix(&e, 'f');
  settle(&e);
  dispatch_key(&e, 'c');
  assert(e.git.mode == GIT_MESSAGE);
  const char *input = "Initial from panel";
  for (int i = 0; input[i]; i++)
    dispatch_key(&e, input[i]);
  dispatch_key(&e, '\r');
  settle(&e);
#ifndef _WIN32
  assert(e.git.failed && e.git.mode == GIT_MESSAGE && e.git.repo.staged == 2);
  assert(!strcmp(e.git.input, input));
  assert(remove(".git/hooks/pre-commit") == 0);
  dispatch_key(&e, '\r');
  settle(&e);
#endif
  assert(!e.git.failed && !e.git.repo.unborn && !e.git.repo.count);
  dispatch_shutdown(&e);

  assert(CHANGE_DIRECTORY("..") == 0);
  editor outside = {.rows = 24, .cols = 100, .running = 1};
  buffer_init(&outside.document.buf);
  dispatch_init(&outside);
  prefix(&outside, 'f');
  assert(outside.sidebar.kind == SIDEBAR_FILES && outside.sidebar.focused);
  prefix(&outside, 'v');
  settle(&outside);
  assert(outside.git.failed && !outside.git.repo.root && !outside.git.repo.files);
  assert(outside.sidebar.kind == SIDEBAR_GIT && outside.sidebar.focused);
  dispatch_key(&outside, CTRL('g'));
  assert(!outside.sidebar.focused);
  prefix(&outside, 't');
  assert(outside.sidebar.kind == SIDEBAR_FILES && file_manager_area(&outside).width > 0);
  dispatch_shutdown(&outside);
  return 0;
}
