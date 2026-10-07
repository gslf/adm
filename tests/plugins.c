#include "plugins.h"
#include "dispatch.h"
#include "fileio.h"
#include "lsp.h"
#include "screen.h"
#include "search.h"
#include "undo.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
static void environment(const char *key, const char *value) {
  assert(!_putenv_s(key, value));
}
#else
static void environment(const char *key, const char *value) {
  assert(!setenv(key, value, 1));
}
#endif
static void invoke(editor *e, int key) {
  dispatch_key(e, META('c'));
  assert(plugins_modal(e));
  dispatch_key(e, key);
  assert(!plugins_modal(e));
}
static int contains(abuf *ab, const char *needle) {
  size_t n = strlen(needle);
  for (int i = 0; i + n <= (size_t)ab->len; i++)
    if (!memcmp(ab->b + i, needle, n))
      return 1;
  return 0;
}
int main(int argc, char **argv) {
  assert(argc == 2);
  const char *good = getenv("ADM_TEST_PLUGIN"),
             *bad = getenv("ADM_TEST_BAD_PLUGIN"),
             *no = getenv("ADM_TEST_NO_PLUGIN");
  assert(good && bad && no);
  FILE *f = fopen("plugins.conf", "wb");
  assert(f);
  const char *functions[] = {"edit_shared",
                             "inspect_lines",
                             "nonexistent_symbol",
                             "edit_shared",
                             "edit_shared",
                             "fail_after_edit",
                             "invalid_unicode_position",
                             "edit_shared"};
  const char *libraries[] = {good, good, good, bad,
                             no,   good, good, "missing.so"};
  for (int i = 0; i < 8; i++)
    fprintf(f,
            "[command %c]\nlabel = Command %d\nlibrary = %s\nfunction = %s\n",
            'a' + i, i, libraries[i], functions[i]);
  fclose(f);
  environment("ADM_CONFIG", "plugins.conf");
  assert(!file_write(argv[1], "original\n"));
  editor e = {
      .document.filename = argv[1], .rows = 24, .cols = 80, .running = 1};
  assert(!buffer_load_text(&e.document.buf, "original"));
  dispatch_register(search_module());
  dispatch_init(&e);
  assert(plugins_count(&e) == 8);
  // Not in the Command Center registry; User Center always works with sidebar
  // focus.
  assert(!dispatch_prefix_find(&e, 'a'));
  dispatch_key(&e, CTRL('b'));
  assert(e.sidebar.focused);
  dispatch_key(&e, META('c'));
  assert(plugins_modal(&e) && e.sidebar.focused);
  abuf ab = {0};
  dispatch_draw(&e, &ab);
  assert(contains(&ab, "USER CENTER") && contains(&ab, "Command 0"));
  ab_free(&ab);
  dispatch_key(&e, '!');
  assert(plugins_modal(&e));
  assert(!strcmp(buffer_line(&e.document.buf, 0), "original"));
  dispatch_key(&e, CTRL('g'));
  assert(!plugins_modal(&e) && e.sidebar.focused);
  // Shared references in another tab and split retain the same document.
  editor_tab *first = e.active_tab;
  dispatch_key(&e, CTRL('t'));
  assert(e.tab_count == 2 && !e.view->doc);
  documents_open(&e, argv[1], NULL);
  assert(e.view->doc == &e.document);
  dispatch_key(&e, CTRL('u'));
  documents_open(&e, argv[1], NULL);
  assert(e.document.views == 3 && e.windows.count == 2);
  invoke(&e, 'a');
  assert(!e.notice[0]);
  assert(!strcmp(buffer_line(&e.document.buf, 0), "plugin"));
  assert(!strcmp(buffer_line(&e.document.buf, 1), "sharedoriginal"));
  assert(first->windows.panes[0].doc == &e.document && e.document.dirty);
  undo_stats stats;
  undo_status(&e.document, &stats);
  assert(stats.undo_actions == 1);
  // All edits made by one C function undo and redo together.
  dispatch_key(&e, CTRL('z'));
  assert(!strcmp(buffer_line(&e.document.buf, 0), "original") &&
         e.document.buf.nlines == 1);
  dispatch_key(&e, META('z'));
  assert(e.document.buf.nlines == 2);
  invoke(&e, 'b');
  assert(!strcmp(e.notice, "Plugin inspected shared lines") &&
         e.view->cy == 1 && e.view->cx == 6);
  invoke(&e, 'c');
  assert(strstr(e.notice, "function not found"));
  invoke(&e, 'd');
  assert(strstr(e.notice, "incompatible ABI"));
  invoke(&e, 'e');
  assert(strstr(e.notice, "incompatible ABI"));
  invoke(&e, 'h');
  assert(strstr(e.notice, "cannot load"));
  assert(!strcmp(buffer_line(&e.document.buf, 0), "plugin"));
  invoke(&e, 'f');
  assert(strstr(e.notice, "failed (7)") &&
         !strcmp(buffer_line(&e.document.buf, 0), "xplugin"));
  dispatch_key(&e, CTRL('z'));
  assert(!strcmp(buffer_line(&e.document.buf, 0), "plugin"));
  e.document.readonly = 1;
  invoke(&e, 'a');
  assert(strstr(e.notice, "failed") &&
         !strcmp(buffer_line(&e.document.buf, 0), "plugin"));
  e.document.readonly = 0;
#ifdef ADM_TEST_ALLOC
  // Vetoed edits never bypass the buffer's allocation/undo-journal safeguards.
  buffer_test_fail_after(0);
  invoke(&e, 'a');
  buffer_test_fail_after(-1);
  assert(!strcmp(buffer_line(&e.document.buf, 0), "plugin"));
  undo_test_io_failure(1);
  invoke(&e, 'a');
  undo_test_io_failure(0);
  assert(!strcmp(buffer_line(&e.document.buf, 0), "plugin"));
#endif
  // Small terminals retain selection, scrolling and menu keys.
  e.rows = 6;
  e.cols = 28;
  dispatch_key(&e, META('c'));
  dispatch_key(&e, KEY_END);
  ab = (abuf){0};
  dispatch_draw(&e, &ab);
  assert(contains(&ab, "Command 7"));
  ab_free(&ab);
  dispatch_key(&e, KEY_HOME);
  dispatch_key(&e, KEY_DOWN);
  dispatch_key(&e, '\r');
  assert(!plugins_modal(&e) &&
         !strcmp(e.notice, "Plugin inspected shared lines"));
  dispatch_key(&e, CTRL('x'));
  dispatch_key(&e, 'm');
  assert(e.move_active);
  dispatch_key(&e, META('c'));
  assert(plugins_modal(&e) && !e.move_active);
  dispatch_key(&e, '\x1b');
  dispatch_key(&e, CTRL('x'));
  assert(e.prefix_active);
  dispatch_key(&e, META('c'));
  assert(plugins_modal(&e) && !e.prefix_active);
  dispatch_key(&e, '\x1b');
  // An active text-entry prompt must keep ownership of M-c.
  dispatch_key(&e, CTRL('s'));
  assert(search_active(&e));
  dispatch_key(&e, META('c'));
  assert(!plugins_modal(&e) && search_active(&e));
  dispatch_key(&e, '\x1b');
  dispatch_key(&e, META('x'));
  assert(lsp_modal(&e));
  dispatch_key(&e, META('c'));
  assert(!plugins_modal(&e) && lsp_modal(&e));
  dispatch_key(&e, '\x1b');
  dispatch_key(&e, CTRL('x'));
  dispatch_key(&e, '?');
  assert(e.help_active);
  dispatch_key(&e, META('c'));
  assert(e.help_active && !plugins_modal(&e));
  dispatch_key(&e, '\x1b');
  dispatch_shutdown(&e);
  assert(!e.plugins);
  // Empty views show the center and reject file-editing commands.
  editor empty = {.rows = 24, .cols = 80, .running = 1};
  dispatch_init(&empty);
  invoke(&empty, 'a');
  assert(strstr(empty.notice, "failed") && !empty.view->doc);
  dispatch_shutdown(&empty);
  // UTF-8/grapheme offsets are validated before mutation.
  editor unicode = {
      .document.filename = argv[1], .rows = 24, .cols = 80, .running = 1};
  assert(!buffer_load_text(&unicode.document.buf, "界text"));
  dispatch_init(&unicode);
  invoke(&unicode, 'g');
  assert(!strcmp(unicode.notice, "UTF-8 positions protected") &&
         !unicode.document.dirty);
  dispatch_shutdown(&unicode);
  environment("ADM_CONFIG", "missing.conf");
  editor defaults = {.rows = 24, .cols = 80};
  dispatch_init(&defaults);
  assert(!plugins_count(&defaults));
  dispatch_key(&defaults, META('c'));
  ab = (abuf){0};
  dispatch_draw(&defaults, &ab);
  assert(contains(&ab, "~/.adm.conf"));
  ab_free(&ab);
  dispatch_key(&defaults, '\r');
  assert(plugins_modal(&defaults));
  dispatch_key(&defaults, '\x1b');
  dispatch_shutdown(&defaults);
  return 0;
}
