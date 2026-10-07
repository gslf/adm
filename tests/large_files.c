#include "shared_views.h"
#include "dispatch.h"
#include "fileio.h"
#include "utf8.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

static size_t count(const buffer *b) {
  size_t result = b->nlines > 1 ? (size_t)b->nlines - 1 : 0;
  for (block *k = b->head; k; k = k->next)
    for (int i = 0; i < k->count; i++)
      for (int j = 0; k->lines[i][j]; j = grapheme_next(k->lines[i], j))
        result++;
  return result;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  FILE *fp = fopen(argv[1], "wb");
  assert(fp);
  const char *line = "row \xc3\xa8 \xe7\x95\x8c e\xcc\x81 "
      "012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789";
  for (int i = 0; i < 300000; i++)
    assert(fprintf(fp, "%s\r\n", line) > 0);
  assert(fclose(fp) == 0);
  editor e = {.document.filename = argv[1], .rows = 40, .cols = 160, .running = 1};
  assert(load(&e.document.buf, argv[1]) == 0);
  assert(e.document.buf.nlines == 300000);
  assert(!strcmp(buffer_line(&e.document.buf, 299999), line));
  size_t total = count(&e.document.buf);
  assert(buffer_char_count(&e.document.buf) == total);
  assert(e.document.buf.chars_valid);
  dispatch_init(&e);
  block *head = e.document.buf.head;
  for (int i = 0; i < 24; i++) {
    assert(test_shared_tab(&e));
    assert(test_shared_split(&e, LAYOUT_VERTICAL));
    assert(test_shared_split(&e, LAYOUT_HORIZONTAL));
    layout_focus(&e, 1);
    assert(test_shared_split(&e, LAYOUT_HORIZONTAL));
    assert(e.view->doc->buf.head == head && e.view->doc == &e.document);
    e.view->cy = 299999;
    dispatch_key(&e, CTRL('a'));
  }
  assert(e.document.views == 97);
  dispatch_key(&e, 'X');
  assert(buffer_char_count(&e.document.buf) == total + 1);
  for (editor_tab *tab = e.tabs; tab; tab = tab->next) {
    workspace *w = tabs_workspace(&e, tab);
    for (int i = 0; i < MAX_PANES; i++)
      if (w->panes[i].doc) {
        assert(w->panes[i].doc->dirty);
        assert(w->panes[i].doc->buf.head == head);
        assert(!strncmp(buffer_line(&w->panes[i].doc->buf, 299999), "Xrow", 4));
      }
  }
  dispatch_key(&e, CTRL('x'));
  dispatch_key(&e, CTRL('s'));
  assert(!e.document.dirty && !e.notice[0]);
  buffer saved;
  assert(load(&saved, argv[1]) == 0);
  assert(saved.nlines == e.document.buf.nlines && buffer_char_count(&saved) == total + 1);
  assert(!strcmp(buffer_line(&saved, 299999), buffer_line(&e.document.buf, 299999)));
  buffer_free(&saved);
  dispatch_key(&e, CTRL('z'));
  assert(e.document.dirty && e.document.buf.head == head);
  assert(buffer_char_count(&e.document.buf) == total);
  assert(!strcmp(buffer_line(&e.document.buf, 299999), line));
  dispatch_key(&e, META('z'));
  assert(!e.document.dirty && e.document.buf.head == head);
  assert(buffer_char_count(&e.document.buf) == total + 1);
  dispatch_shutdown(&e);
  assert(!e.document.buf.head && !e.document.views);

  // Cache correctness across block boundaries, joins, removals, and UTF-8 byte edits.
  buffer b;
  buffer_init(&b);
  for (int i = 0; i < 1100; i++)
    assert(buffer_insert_newline(&b, i, 0) == 0);
  for (int i = 0; i < 500; i++) {
    int row = (i * 137) % b.nlines;
    switch (i % 5) {
    case 0:
      assert(buffer_insert_char(&b, row, 0, 'X') == 0);
      break;
    case 1:
      assert(buffer_insert_newline(&b, row, 0) == 0);
      break;
    case 2:
      if (row + 1 < b.nlines)
        assert(buffer_join_line(&b, row) == 0);
      break;
    case 3:
      buffer_remove_line(&b, row);
      break;
    case 4:
      assert(buffer_insert_char(&b, row, 0, (char)0xc3) == 0);
      assert(buffer_char_count(&b) == count(&b));
      assert(buffer_insert_char(&b, row, 1, (char)0xa8) == 0);
      assert(buffer_char_count(&b) == count(&b));
      assert(buffer_delete_char(&b, row, 0) == 0);
      break;
    }
    assert(buffer_char_count(&b) == count(&b));
    assert(buffer_char_count(&b) == count(&b));
    for (int j = b.nlines - 1; j >= 0; j -= 73)
      assert(buffer_line(&b, j));
  }
  buffer_free(&b);

  // Chunk boundaries, long lines, BOM and CRLF match the in-memory loader.
  size_t length = 3 * 65536 + 7;
  char *text = malloc(length + 10);
  assert(text);
  memcpy(text, "\xef\xbb\xbf", 3);
  memset(text + 3, 'a', length - 3);
  memcpy(text + length, "\r\nlast\r", 8);
  assert(file_write(argv[1], text) == 0);
  buffer expected;
  assert(buffer_load_text(&expected, text) == 0 && load(&b, argv[1]) == 0);
  assert(b.nlines == expected.nlines);
  for (int i = 0; i < b.nlines; i++)
    assert(!strcmp(buffer_line(&b, i), buffer_line(&expected, i)));
  buffer_free(&b);
  buffer_free(&expected);
  free(text);
  fp = fopen(argv[1], "wb");
  assert(fp && fwrite("bad\0data", 1, 8, fp) == 8 && fclose(fp) == 0);
  assert(load(&b, argv[1]) < 0 && !b.head && !b.nlines);

  // Save failures retain dirty data and report the failure to the user.
  editor failure = {.document.filename = "/missing-adm-directory/file", .rows = 24, .cols = 80};
  buffer_init(&failure.document.buf);
  dispatch_init(&failure);
  dispatch_key(&failure, 'Q');
  dispatch_key(&failure, CTRL('x'));
  dispatch_key(&failure, CTRL('s'));
  assert(failure.document.dirty && failure.notice[0]);
  assert(!strcmp(buffer_line(&failure.document.buf, 0), "Q"));
  dispatch_shutdown(&failure);
  puts("Large-file regressions passed.");
  return 0;
}
