#include "undo.h"
#include "clipboard.h"
#include "cursor.h"
#include "documents.h"
#include "fileio.h"
#include "search.h"
#include "shared_views.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
static void prefix(editor *e, int key) {
  dispatch_key(e, CTRL('x'));
  dispatch_key(e, key);
  assert(!e->prefix_active);
}
static void type(editor *e, const char *s) {
  while (*s)
    dispatch_key(e, *s == '\n' ? (++s, '\r') : (unsigned char)*s++);
}
static void content(const document *doc, const char *text) {
  const char *p = text;
  for (int row = 0; row < doc->buf.nlines; row++) {
    const char *end = strchr(p, '\n');
    size_t n = end ? (size_t)(end - p) : strlen(p);
    const char *line = buffer_line(&doc->buf, row);
    assert(strlen(line) == n && !memcmp(line, p, n));
    if (!end) {
      assert(row + 1 == doc->buf.nlines);
      p += n;
      break;
    }
    p = end + 1;
  }
  assert(!*p);
}
static void reset(editor *e, const char *text) {
  document *doc = e->view->doc;
  undo_clear(doc);
  buffer_free(&doc->buf);
  assert(buffer_load_text(&doc->buf, text) == 0);
  doc->dirty = 0;
  for (editor_tab *t = e->tabs; t; t = t->next) {
    workspace *w = tabs_workspace(e, t);
    for (int i = 0; i < MAX_PANES; i++)
      if (w->panes[i].doc == doc) {
        view *v = &w->panes[i];
        v->cx = v->cy = v->selx = v->sely = v->sel_active = v->sel_mode = 0;
        v->sticky = v->rowoff = v->coloff = 0;
      }
  }
  layout_bind_view(e, e->view, doc);
  e->sidebar.focused = 0;
}
static void wait_search(editor *e, unsigned long long expected) {
  unsigned long long n = 0;
  int done = 0;
  for (int i = 0; i < 10000 && !done; i++) {
    search_status(e, &n, &done);
#ifdef _WIN32
    Sleep(1);
#else
    usleep(1000);
#endif
  }
  assert(done && n == expected);
}
static void replay_faults(editor *e) {
  reset(e, "one\ntwo\nthree");
  undo_group_begin(e);
  assert(buffer_replace_range(&e->view->doc->buf, 0, 0, 3, "ONE") == 0);
  dispatch_change(e);
  assert(buffer_replace_range(&e->view->doc->buf, 1, 0, 3, "TWO") == 0);
  dispatch_change(e);
  assert(buffer_replace_range(&e->view->doc->buf, 2, 0, 5, "THREE") == 0);
  dispatch_change(e);
  undo_group_end(e);
  undo_stats stats;
  undo_status(e->view->doc, &stats);
  assert(stats.undo_actions == 1 && stats.records == 3);
  for (int i = 0; i < 6; i++) {
    buffer_test_fail_after(i);
    dispatch_key(e, CTRL('z'));
    buffer_test_fail_after(-1);
    content(e->view->doc, "ONE\nTWO\nTHREE");
    assert(e->view->doc->dirty && strstr(e->notice, "unchanged"));
    undo_status(e->view->doc, &stats);
    assert(stats.undo_actions == 1 && !stats.redo_actions);
  }
  dispatch_key(e, CTRL('z'));
  content(e->view->doc, "one\ntwo\nthree");
  assert(!e->view->doc->dirty);
  for (int i = 0; i < 6; i++) {
    buffer_test_fail_after(i);
    dispatch_key(e, META('z'));
    buffer_test_fail_after(-1);
    content(e->view->doc, "one\ntwo\nthree");
    assert(!e->view->doc->dirty && strstr(e->notice, "unchanged"));
  }
  dispatch_key(e, META('z'));
  content(e->view->doc, "ONE\nTWO\nTHREE");
  undo_test_io_failure(1);
  dispatch_key(e, CTRL('z'));
  undo_test_io_failure(0);
  content(e->view->doc, "ONE\nTWO\nTHREE");
  assert(strstr(e->notice, "unchanged"));
  reset(e, "");
  undo_test_io_failure(1);
  dispatch_key(e, 'X');
  undo_test_io_failure(0);
  assert(!e->view->doc->buf.nlines && !e->view->doc->dirty);
  assert(strstr(e->notice, "edit cancelled"));
  dispatch_key(e, 'X');
  dispatch_key(e, CTRL('z'));
  undo_test_io_failure(1);
  dispatch_key(e, 'Y');
  undo_test_io_failure(0);
  assert(!e->view->doc->buf.nlines);
  undo_status(e->view->doc, &stats);
  assert(stats.redo_actions == 1 && !stats.undo_actions);
  dispatch_key(e, META('z'));
  content(e->view->doc, "X");
}
static char *snapshot(const buffer *b) {
  size_t size = 1;
  for (int row = 0; row < b->nlines; row++)
    size += strlen(buffer_line(b, row)) + 1;
  char *text = malloc(size);
  assert(text);
  size_t n = 0;
  for (int row = 0; row < b->nlines; row++) {
    if (row)
      text[n++] = '\n';
    size_t len = strlen(buffer_line(b, row));
    memcpy(text + n, buffer_line(b, row), len);
    n += len;
  }
  text[n] = 0;
  return text;
}
static unsigned random_state = 0x5a17;
static unsigned next_random(unsigned bound) {
  random_state = random_state * 1664525u + 1013904223u;
  return random_state % bound;
}
static void mixed_transactions(editor *e) {
  const char *texts[] = {"", "X", "ab", "\n", "Y\nZ", "\n\n"};
  for (int iteration = 0; iteration < 120; iteration++) {
    reset(e, "first\nsecond\nthird\nlast");
    char *before = snapshot(&e->view->doc->buf);
    undo_group_begin(e);
    for (int i = 0; i < 15; i++) {
      buffer *b = &e->view->doc->buf;
      int row = (int)next_random((unsigned)b->nlines);
      int end = row + (int)next_random((unsigned)(b->nlines - row));
      int col = (int)next_random((unsigned)strlen(buffer_line(b, row)) + 1);
      int ec = (int)next_random((unsigned)strlen(buffer_line(b, end)) + 1);
      if (row == end && ec < col) {
        int swap = col;
        col = ec;
        ec = swap;
      }
      const char *text = texts[next_random(6)];
      assert(buffer_replace_span(b, row, col, end, ec, text, strlen(text),
                                 NULL) == 0);
      dispatch_change(e);
    }
    undo_group_end(e);
    char *after = snapshot(&e->view->doc->buf);
    buffer_test_fail_after(5);
    dispatch_key(e, CTRL('z'));
    buffer_test_fail_after(-1);
    content(e->view->doc, after);
    dispatch_key(e, CTRL('z'));
    content(e->view->doc, before);
    dispatch_key(e, META('z'));
    content(e->view->doc, after);
    free(before);
    free(after);
  }
  // Many edits in one group retain only changed lines, including on replay.
  char *text = malloc(100000);
  assert(text);
  size_t n = 0;
  for (int i = 0; i < 10000; i++)
    n += (size_t)sprintf(text + n, "%srow%d", i ? "\n" : "", i);
  reset(e, text);
  undo_group_begin(e);
  for (int row = 0; row < 10000; row++)
    assert(buffer_replace_range(&e->view->doc->buf, row, 0, 1, "R") == 0);
  undo_group_end(e);
  dispatch_key(e, CTRL('z'));
  content(e->view->doc, text);
  dispatch_key(e, META('z'));
  for (int row = 0; row < 10000; row++)
    assert(buffer_line(&e->view->doc->buf, row)[0] == 'R');
  free(text);
}
static void journal_reclamation(editor *e) {
  char text[4097];
  memset(text, 'a', sizeof text - 1);
  text[4096] = 0;
  reset(e, text);
  for (int i = 0; i < 2200; i++) {
    memset(text, i % 2 ? 'a' : 'b', sizeof text - 1);
    assert(buffer_replace_range(&e->view->doc->buf, 0, 0, 4096, text) == 0);
    dispatch_change(e);
  }
  undo_stats stats;
  undo_status(e->view->doc, &stats);
  assert(stats.undo_actions == 1000 &&
         stats.journal_bytes < 12u * 1024u * 1024u);
  for (int i = 0; i < 1000; i++) {
    dispatch_key(e, CTRL('z'));
    assert(buffer_line(&e->view->doc->buf, 0)[0] == (i % 2 ? 'a' : 'b'));
  }
  for (int i = 0; i < 1000; i++) {
    dispatch_key(e, META('z'));
    assert(buffer_line(&e->view->doc->buf, 0)[0] == (i % 2 ? 'a' : 'b'));
  }
}
int main(int argc, char **argv) {
  assert(argc == 2 && file_write(argv[1], "") == 0);
  editor e = {
      .document.filename = argv[1], .rows = 40, .cols = 160, .running = 1};
  buffer_init(&e.document.buf);
  dispatch_register(clipboard_module());
  dispatch_register(search_module());
  dispatch_init(&e);
  type(&e, "abc");
  undo_stats stats;
  undo_status(&e.document, &stats);
  assert(stats.undo_actions == 1 && stats.records == 1);
  dispatch_key(&e, CTRL('z'));
  assert(!e.document.buf.nlines && !e.document.dirty);
  dispatch_key(&e, META('z'));
  content(&e.document, "abc");
  assert(e.document.dirty);
  prefix(&e, CTRL('s'));
  assert(!e.document.dirty);
  prefix(&e, 'z');
  assert(!e.document.buf.nlines && e.document.dirty);
  prefix(&e, 'Z');
  content(&e.document, "abc");
  assert(!e.document.dirty);
  dispatch_key(&e, CTRL('z'));
  type(&e, "new");
  dispatch_key(&e, META('z'));
  content(&e.document, "new");
  assert(strstr(e.notice, "Nothing"));
  reset(&e, "");
  dispatch_key(&e, '\r');
  assert(e.document.buf.nlines == 2);
  dispatch_key(&e, CTRL('z'));
  assert(e.document.buf.nlines == 0);
  dispatch_key(&e, META('z'));
  assert(e.document.buf.nlines == 2);
  reset(&e, "");
  dispatch_key(&e, 0xe9);
  dispatch_key(&e, 0x301);
  dispatch_key(&e, 0x754c);
  content(&e.document, "é́界");
  dispatch_key(&e, CTRL('z'));
  assert(!e.document.buf.nlines);
  dispatch_key(&e, META('z'));
  content(&e.document, "é́界");
  dispatch_key(&e, KEY_BACKSPACE);
  dispatch_key(&e, KEY_BACKSPACE);
  content(&e.document, "");
  dispatch_key(&e, CTRL('z'));
  content(&e.document, "é́界");
  dispatch_key(&e, KEY_HOME);
  dispatch_key(&e, CTRL('d'));
  content(&e.document, "界");
  dispatch_key(&e, CTRL('z'));
  content(&e.document, "é́界");
  reset(&e, "a\nb");
  e.view->cy = 1;
  dispatch_key(&e, KEY_BACKSPACE);
  content(&e.document, "ab");
  dispatch_key(&e, CTRL('z'));
  content(&e.document, "a\nb");
  dispatch_key(&e, META('z'));
  content(&e.document, "ab");
  reset(&e, "one\ntwo\nthree");
  e.view->sely = 0;
  e.view->selx = 1;
  e.view->cy = 2;
  e.view->cx = 2;
  e.view->sel_active = e.view->sel_mode = 1;
  dispatch_key(&e, CTRL('w'));
  content(&e.document, "oree");
  dispatch_key(&e, CTRL('z'));
  content(&e.document, "one\ntwo\nthree");
  assert(e.view->sel_active && e.view->cx == 2 && e.view->cy == 2);
  dispatch_key(&e, CTRL('g'));
  dispatch_key(&e, META('<'));
  dispatch_key(&e, CTRL('y'));
  content(&e.document, "ne\ntwo\nthone\ntwo\nthree");
  dispatch_key(&e, CTRL('z'));
  content(&e.document, "one\ntwo\nthree");
  dispatch_key(&e, META('z'));
  content(&e.document, "ne\ntwo\nthone\ntwo\nthree");
  reset(&e, "one\ntwo");
  e.view->sely = 0;
  e.view->selx = 1;
  e.view->cy = 1;
  e.view->cx = 2;
  e.view->sel_active = 1;
  dispatch_key(&e, 'X');
  content(&e.document, "oXo");
  dispatch_key(&e, CTRL('z'));
  content(&e.document, "one\ntwo");
  // Shared history survives edits, undo and redo from other tabs and splits.
  reset(&e, "");
  type(&e, "abc");
  assert(test_shared_tab(&e) && test_shared_split(&e, LAYOUT_VERTICAL));
  dispatch_key(&e, CTRL('z'));
  assert(!e.document.buf.nlines && !e.document.dirty);
  tabs_focus(&e, -1);
  e.sidebar.focused = 0;
  dispatch_key(&e, META('z'));
  content(&e.document, "abc");
  assert(e.document.views == 3);
  char *other = malloc(strlen(argv[1]) + 7);
  assert(other);
  sprintf(other, "%s-other", argv[1]);
  assert(file_write(other, "other") == 0);
  documents_open(&e, other, NULL);
  document *different = e.view->doc;
  assert(different != &e.document);
  dispatch_key(&e, 'Z');
  dispatch_key(&e, CTRL('z'));
  content(different, "other");
  content(&e.document, "abc");
  documents_open(&e, argv[1], NULL);
  assert(!documents_find(&e, other));
  free(other);
  // Replace-all is one transaction; its precise events preserve shared cursors.
  reset(&e, "aa keep aa\nnext aa");
  tabs_focus(&e, 1);
  e.sidebar.focused = 0;
  e.view->cx = 6;
  editor_tab *watch_tab = e.active_tab;
  int watch_index = e.windows.active;
  tabs_focus(&e, -1);
  view *watch = &tabs_workspace(&e, watch_tab)->panes[watch_index];
  e.sidebar.focused = 0;
  prefix(&e, 'r');
  type(&e, "aa");
  wait_search(&e, 3);
  dispatch_key(&e, '\r');
  type(&e, "X");
  dispatch_key(&e, '\r');
  dispatch_key(&e, 'a');
  for (int i = 0; i < 20 && strcmp(buffer_line(&e.document.buf, 1), "next X");
       i++)
    dispatch_tick(&e);
  dispatch_tick(&e);
  dispatch_key(&e, CTRL('g'));
  content(&e.document, "X keep X\nnext X");
  assert(watch->cx == 5);
  dispatch_key(&e, CTRL('z'));
  content(&e.document, "aa keep aa\nnext aa");
  assert(watch->cx == 6);
  dispatch_key(&e, META('z'));
  content(&e.document, "X keep X\nnext X");
  assert(watch->cx == 5);
  // No-op replacements must retain the saved marker and empty history.
  reset(&e, "aa keep aa\nnext aa");
  prefix(&e, 'r');
  type(&e, "aa");
  wait_search(&e, 3);
  dispatch_key(&e, '\r');
  type(&e, "aa");
  dispatch_key(&e, '\r');
  dispatch_key(&e, 'a');
  for (int i = 0; i < 5; i++)
    dispatch_tick(&e);
  dispatch_key(&e, CTRL('g'));
  assert(!e.document.dirty);
  undo_status(&e.document, &stats);
  assert(!stats.undo_actions);
  reset(&e, "a\nb\nc");
  buffer_remove_line(&e.document.buf, 1);
  dispatch_change(&e);
  content(&e.document, "a\nc");
  dispatch_key(&e, CTRL('z'));
  content(&e.document, "a\nb\nc");
  dispatch_key(&e, META('z'));
  content(&e.document, "a\nc");
  reset(&e, "only");
  buffer_remove_line(&e.document.buf, 0);
  dispatch_change(&e);
  assert(!e.document.buf.nlines);
  dispatch_key(&e, CTRL('z'));
  content(&e.document, "only");
  dispatch_key(&e, META('z'));
  assert(!e.document.buf.nlines);
  replay_faults(&e);
  mixed_transactions(&e);
  journal_reclamation(&e);
  // Block-boundary splices preserve unaffected storage and cache correctness.
  char *many = malloc(40000);
  assert(many);
  size_t used = 0;
  for (int i = 0; i < 2200; i++)
    used += (size_t)sprintf(many + used, "%srow%d", i ? "\n" : "", i);
  reset(&e, many);
  char *untouched = buffer_line(&e.document.buf, 1500);
  e.view->sely = 1022;
  e.view->selx = 2;
  e.view->cy = 1026;
  e.view->cx = 3;
  e.view->sel_active = 1;
  dispatch_key(&e, CTRL('w'));
  dispatch_key(&e, CTRL('z'));
  content(&e.document, many);
  assert(buffer_line(&e.document.buf, 1500) == untouched);
  assert(buffer_char_count(&e.document.buf) == strlen(many));
  free(many);
  reset(&e, "");
  for (int i = 0; i < 1100; i++)
    dispatch_key(&e, '.');
  undo_status(&e.document, &stats);
  assert(stats.undo_actions == 1000);
  for (int i = 0; i < 1000; i++)
    dispatch_key(&e, CTRL('z'));
  assert(strlen(buffer_line(&e.document.buf, 0)) == 100);
  // Reloading saved text invalidates history, preserving the shared document.
  prefix(&e, CTRL('s'));
  char *parent = malloc(strlen(argv[1]) + 1);
  assert(parent);
  strcpy(parent, argv[1]);
  char *separator = strrchr(parent, '/');
#ifdef _WIN32
  char *backslash = strrchr(parent, '\\');
  if (backslash && (!separator || backslash > separator))
    separator = backslash;
#endif
  assert(separator);
  *separator = 0;
  documents_reload(&e, parent);
  free(parent);
  assert(!e.document.history);
  assert(!e.document.dirty);
  dispatch_shutdown(&e);
  assert(!e.document.history && !e.document.views && !e.document.buf.head);
  puts("Undo regressions passed.");
  return 0;
}
