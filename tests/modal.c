#include "dispatch.h"
#include "fileio.h"
#include "lsp.h"
#include "path.h"
#include "plugins.h"
#include "screen.h"
#include "search.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

enum {
  EDITOR, EMPTY, FILES, DIFF, MOVE, HELP, USER_CENTER, LSP_MENU, LSP_HELP,
  LSP_RENAME, SEARCH, INVALID_REGEX, GOTO, REPLACE_NAME, REPLACE_ACTIONS,
  FILE_REPLACING, WORKSPACE_SEARCH, WORKSPACE_RESULTS, WORKSPACE_CONFIRM,
  WORKSPACE_REPLACING, NEW_FILE, RENAME_FILE, DELETE_FILE, OPEN_CONFIRM,
  QUIT_CONFIRM, CALLBACK_CONFIRM, GIT_MESSAGE_INPUT, GIT_BRANCH_INPUT,
  GIT_CHECKOUT_PICKER, GIT_MERGE_PICKER, GIT_WORKING, COMPACT, MODE_COUNT
};
static int cancelled, accepted;
static void cancel_confirmation(editor *e) { (void)e; cancelled++; }
static void accept_confirmation(editor *e) { (void)e; accepted++; }

static void prefix(editor *e, int key) {
  dispatch_key(e, CTRL('x'));
  assert(e->prefix_active);
  dispatch_key(e, key);
  assert(!e->prefix_active);
}
static void type(editor *e, const char *text) {
  while (*text) dispatch_key(e, (unsigned char)*text++);
}
static void wait_search(editor *e) {
  int done = 0;
  unsigned long long matches;
  for (int i = 0; i < 10000 && !done; i++) {
    search_status(e, &matches, &done);
#ifdef _WIN32
    Sleep(1);
#else
    usleep(1000);
#endif
  }
  assert(done);
}
static void select_file(editor *e) {
  sidebar_show(e, SIDEBAR_FILES);
  for (int i = 0; i < e->files.tree.count; i++)
    if (!strcmp(path_name(e->files.tree.entries[i].path), "document.c")) {
      e->files.tree.selected = i;
      return;
    }
  assert(!"Missing modal fixture");
}

static void setup(editor *e, int mode) {
  if (mode == EMPTY) layout_set_document(e, NULL);
  else if (mode == FILES) select_file(e);
  else if (mode == DIFF) documents_preview(e, e->windows.active, "Diff", "diff\n", NULL);
  else if (mode == MOVE) prefix(e, 'm');
  else if (mode == HELP) prefix(e, '?');
  else if (mode == USER_CENTER) dispatch_key(e, META('c'));
  else if (mode == LSP_MENU || mode == LSP_HELP || mode == LSP_RENAME) {
    dispatch_key(e, META('x'));
    if (mode != LSP_MENU) dispatch_key(e, mode == LSP_HELP ? '?' : 'N');
    assert(lsp_modal(e));
  } else if (mode >= SEARCH && mode <= WORKSPACE_REPLACING) {
    int workspace = mode >= WORKSPACE_SEARCH;
    prefix(e, mode == GOTO ? 'l' : workspace ? mode >= WORKSPACE_CONFIRM ? 'R' : 'S' :
           mode >= REPLACE_NAME ? 'r' : 's');
    if (mode == GOTO) type(e, "2");
    else {
      if (mode == INVALID_REGEX) dispatch_key(e, '\t');
      type(e, mode == INVALID_REGEX ? "[" : "needle");
      wait_search(e);
      if (mode == REPLACE_NAME || mode == REPLACE_ACTIONS || mode == FILE_REPLACING ||
          mode == WORKSPACE_RESULTS || mode >= WORKSPACE_CONFIRM)
        dispatch_key(e, '\r');
      if (mode == REPLACE_NAME || mode == REPLACE_ACTIONS || mode == FILE_REPLACING ||
          mode >= WORKSPACE_CONFIRM) {
        type(e, "X");
        if (mode != REPLACE_NAME) dispatch_key(e, '\r');
        if (mode == FILE_REPLACING || mode >= WORKSPACE_CONFIRM) {
          dispatch_key(e, 'a');
          if (mode == FILE_REPLACING) {
            dispatch_tick(e);
            assert(e->document.dirty && !strcmp(buffer_line(&e->document.buf, 0), "X X"));
          } else {
            assert(e->confirmation);
            if (mode == WORKSPACE_REPLACING) dispatch_key(e, 'y');
          }
        }
      }
    }
    assert(search_active(e));
  } else if (mode == NEW_FILE) {
    dispatch_key(e, CTRL('u'));
    prefix(e, 'N');
    type(e, "not-created.c");
    assert(e->new_file.active);
  } else if (mode == RENAME_FILE || mode == DELETE_FILE) {
    select_file(e);
    prefix(e, mode == RENAME_FILE ? 'n' : 'd');
    assert(mode == RENAME_FILE ? e->new_file.active : e->confirmation != NULL);
  } else if (mode == OPEN_CONFIRM) {
    assert(!file_write("other.c", "other\n"));
    e->document.dirty = 1;
    documents_open(e, "other.c", NULL);
    assert(e->confirmation && e->open_request.path);
  } else if (mode == QUIT_CONFIRM) {
    e->document.dirty = 1;
    prefix(e, CTRL('c'));
    assert(e->confirmation);
  } else if (mode == CALLBACK_CONFIRM)
    dispatch_confirm_with_cancel(e, accept_confirmation, cancel_confirmation, "Test confirmation?");
  else if (mode >= GIT_MESSAGE_INPUT && mode <= GIT_WORKING) {
    e->sidebar.kind = SIDEBAR_GIT;
    e->sidebar.focused = 1;
    e->git.mode = mode == GIT_MESSAGE_INPUT ? GIT_MESSAGE :
                  mode == GIT_BRANCH_INPUT ? GIT_NEW_BRANCH :
                  mode == GIT_MERGE_PICKER ? GIT_PICK_MERGE : GIT_PICK_CHECKOUT;
    if (mode == GIT_WORKING) e->git.action = GIT_BRANCHES;
  } else if (mode == COMPACT) {
    e->cols = e->rows = 1;
    layout_arrange(e);
    prefix(e, '?');
  }
}

int main(int argc, char **argv) {
  assert(argc == 2);
  dispatch_register(search_module());
  const int keys[] = {'\x1b', CTRL('x'), META('x'), CTRL('c')};
  for (int mode = 0; mode < MODE_COUNT; mode++)
    for (size_t k = 0; k < sizeof keys / sizeof *keys; k++) {
      assert(!file_write(argv[1], "needle needle\nneedle\n"));
      editor e = {.document.filename = argv[1], .rows = 80, .cols = 160, .running = 1};
      assert(!load(&e.document.buf, argv[1]));
      dispatch_init(&e);
      cancelled = accepted = 0;
      setup(&e, mode);
      if (keys[k] == CTRL('c')) {
        // Quit directly from the original mode, including an active search.
        prefix(&e, CTRL('c'));
        assert(!search_active(&e) && !lsp_modal(&e) && !e.new_file.active);
        if (e.confirmation) {
          assert(e.running);
          dispatch_key(&e, '\x1b');
          assert(e.running && !e.confirmation);
          prefix(&e, CTRL('c'));
          dispatch_key(&e, 'y');
        }
        assert(!e.running);
        e.git.action = GIT_IDLE;
        dispatch_shutdown(&e);
        remove("other.c");
        continue;
      }
      document *doc = e.view->doc;
      int dirty = doc ? doc->dirty : 0;
      char text[80] = {0};
      if (doc) snprintf(text, sizeof text, "%s", buffer_line(&doc->buf, 0));
      dispatch_key(&e, keys[k]);
      if (e.new_file.active || e.confirmation || e.move_active || e.help_active ||
          plugins_modal(&e) || search_active(&e))
        fprintf(stderr, "Modal regression failed: mode=%d key=%d\n", mode, keys[k]);
      assert(!e.new_file.active && !e.new_file.directory && !e.files.delete_path);
      assert(!e.confirmation && !e.confirmation_cancel && !e.open_request.path);
      assert(!e.move_active && !e.help_active && !plugins_modal(&e) && !search_active(&e));
      assert(e.git.mode == GIT_FILES && !accepted);
      assert(cancelled == (mode == CALLBACK_CONFIRM));
      if (mode != WORKSPACE_REPLACING && doc) {
        assert(doc == e.view->doc && doc->dirty == dirty);
        assert(!strcmp(buffer_line(&doc->buf, 0), text));
      }
      assert(e.prefix_active == (keys[k] == CTRL('x')));
      assert(lsp_modal(&e) == (keys[k] == META('x')));
      if (keys[k] != '\x1b' && mode != COMPACT) {
        abuf menu = {0};
        dispatch_draw(&e, &menu);
        ab_append(&menu, "", 1);
        assert(strstr(menu.b, keys[k] == CTRL('x') ? "Command Center" : " LSP  M-x"));
        ab_free(&menu);
      }
      // Either menu can replace the other, even after a text-entry prompt.
      dispatch_key(&e, META('x'));
      assert(lsp_modal(&e) && !e.prefix_active);
      dispatch_key(&e, CTRL('x'));
      assert(e.prefix_active && !lsp_modal(&e));
      dispatch_key(&e, '\x1b');
      assert(!e.prefix_active && !lsp_modal(&e));
      if (mode == FILE_REPLACING) {
        dispatch_key(&e, CTRL('z'));
        assert(!strcmp(buffer_line(&e.document.buf, 0), "needle needle"));
      }
      // The application's existing quit command is reachable from every mode.
      prefix(&e, CTRL('c'));
      if (e.confirmation) {
        assert(e.running);
        dispatch_key(&e, 'y');
      }
      assert(!e.running);
      e.git.action = GIT_IDLE;
      dispatch_shutdown(&e);
      remove("other.c");
    }
  return 0;
}
