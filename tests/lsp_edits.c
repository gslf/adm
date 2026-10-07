#include "lsp_edits.h"
#include "fileio.h"
#include "lsp.h"
#include "shared_views.h"
#include "undo.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void edits(json_buf *b, const char *uri, int end, const char *text) {
  json_quote(b, uri);
  json_add(b, ":[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{"
              "\"line\":0,\"character\":");
  json_number(b, end);
  json_add(b, "}},\"newText\":");
  json_quote(b, text);
  json_add(b, "}]");
}
static int apply(editor *e, const json_buf *b, const lsp_edit_version *versions,
                 size_t n) {
  json_value v = {0};
  assert(json_parse(&v, b->data, b->length));
  int result = lsp_apply_edits(e, &v, 0, NULL, 0, versions, n);
  json_free(&v);
  return result;
}
static void unchanged(editor *e, int tabs) {
  assert(!strcmp(buffer_line(&e->document.buf, 0), "print"));
  assert(e->tab_count == tabs && !e->document.dirty);
  undo_stats stats;
  undo_status(&e->document, &stats);
  assert(!stats.undo_actions && !stats.redo_actions);
}
static void precise_positions(void) {
  editor e = {.document.filename = "precise.c", .rows = 24, .cols = 80};
  assert(buffer_load_text(&e.document.buf, "aa middle zz") == 0);
  dispatch_init(&e);
  assert(test_shared_split(&e, LAYOUT_VERTICAL));
  view *first = &e.windows.panes[0], *second = e.view;
  first->cx = second->cx = 5;
  first->selx = 4;
  first->sel_active = 1;
  char *uri = lsp_uri(e.document.filename);
  lsp_edit_version version = {uri, e.document.change_id};
  const char *text =
      "[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
      "\"end\":{\"line\":0,\"character\":0}},\"newText\":\"X\"},"
      "{\"range\":{\"start\":{\"line\":0,\"character\":12},"
      "\"end\":{\"line\":0,\"character\":12}},\"newText\":\"Y\"}]";
  json_value v = {0};
  assert(json_parse(&v, text, strlen(text)));
  assert(lsp_apply_edits(&e, &v, 0, uri, 0, &version, 1));
  assert(!strcmp(buffer_line(&e.document.buf, 0), "Xaa middle zzY"));
  assert(first->cx == 6 && second->cx == 6 && first->selx == 5);
  dispatch_key(&e, CTRL('z'));
  assert(first->cx == 5 && second->cx == 5 && first->selx == 4);
  dispatch_key(&e, META('z'));
  assert(first->cx == 6 && second->cx == 6 && first->selx == 5);
  json_free(&v);
  version.generation = e.document.change_id;
  const char *whole =
      "[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
      "\"end\":{\"line\":1,\"character\":0}},\"newText\":\"formatted\\n\"}]";
  assert(json_parse(&v, whole, strlen(whole)));
  assert(lsp_apply_edits(&e, &v, 0, uri, 0, &version, 1));
  assert(e.document.buf.nlines == 1 &&
         !strcmp(buffer_line(&e.document.buf, 0), "formatted"));
  dispatch_key(&e, CTRL('z'));
  assert(e.document.buf.nlines == 1 &&
         !strcmp(buffer_line(&e.document.buf, 0), "Xaa middle zzY"));
  dispatch_key(&e, META('z'));
  assert(e.document.buf.nlines == 1 &&
         !strcmp(buffer_line(&e.document.buf, 0), "formatted"));
  json_free(&v);
  free(uri);
  dispatch_shutdown(&e);
}
static void dense_edits(void) {
  const int lines = 4096;
  char *original = malloc((size_t)lines * 2 + 1);
  assert(original);
  for (int i = 0; i < lines; i++) {
    original[2 * i] = 'v';
    original[2 * i + 1] = '\n';
  }
  original[lines * 2] = 0;
  editor e = {.document.filename = "dense.c", .rows = 24, .cols = 80};
  assert(buffer_load_text(&e.document.buf, original) == 0);
  free(original);
  dispatch_init(&e);
  char *uri = lsp_uri(e.document.filename);
  lsp_edit_version version = {uri, e.document.change_id};
  json_buf b = {0};
  json_add(&b, "[");
  for (int i = 0; i < lines; i++) {
    if (i)
      json_add(&b, ",");
    json_add(&b, "{\"range\":{\"start\":{\"line\":");
    json_number(&b, i);
    json_add(&b, ",\"character\":0},\"end\":{\"line\":");
    json_number(&b, i);
    json_add(&b, ",\"character\":1}},\"newText\":\"long\"}");
  }
  json_add(&b, "]");
  json_value v = {0};
  assert(json_parse(&v, b.data, b.length));
  assert(lsp_apply_edits(&e, &v, 0, uri, 0, &version, 1));
  assert(e.document.buf.nlines == lines);
  for (int i = 0; i < lines; i++)
    assert(!strcmp(buffer_line(&e.document.buf, i), "long"));
  dispatch_key(&e, CTRL('z'));
  for (int i = 0; i < lines; i++)
    assert(!strcmp(buffer_line(&e.document.buf, i), "v"));
  dispatch_key(&e, META('z'));
  for (int i = 0; i < lines; i++)
    assert(!strcmp(buffer_line(&e.document.buf, i), "long"));
  json_free(&v);
  json_buf_free(&b);
  free(uri);
  dispatch_shutdown(&e);
}
static void unicode_edits(void) {
  const char *original = "è😀 abc è😀 end";
  editor e = {.document.filename = "unicode.c", .rows = 24, .cols = 80};
  assert(buffer_load_text(&e.document.buf, original) == 0);
  dispatch_init(&e);
  char *uri = lsp_uri(e.document.filename);
  lsp_edit_version version = {uri, e.document.change_id};
  const char *unsorted =
      "[{\"range\":{\"start\":{\"line\":0,\"character\":12},"
      "\"end\":{\"line\":0,\"character\":15}},\"newText\":\"last\"},"
      "{\"range\":{\"start\":{\"line\":0,\"character\":4},"
      "\"end\":{\"line\":0,\"character\":7}},\"newText\":\"word\"}]";
  json_value v = {0};
  assert(json_parse(&v, unsorted, strlen(unsorted)));
  assert(lsp_apply_edits(&e, &v, 0, uri, 0, &version, 1));
  assert(!strcmp(buffer_line(&e.document.buf, 0), "è😀 word è😀 last"));
  dispatch_key(&e, CTRL('z'));
  assert(!strcmp(buffer_line(&e.document.buf, 0), original));
  json_free(&v);
  const char *split_surrogate =
      "[{\"range\":{\"start\":{\"line\":0,\"character\":2},"
      "\"end\":{\"line\":0,\"character\":2}},\"newText\":\"invalid\"}]";
  assert(json_parse(&v, split_surrogate, strlen(split_surrogate)));
  version.generation = e.document.change_id;
  assert(!lsp_apply_edits(&e, &v, 0, uri, 0, &version, 1));
  assert(!strcmp(buffer_line(&e.document.buf, 0), original));
  json_free(&v);
  const char *split_byte =
      "[{\"range\":{\"start\":{\"line\":0,\"character\":3},"
      "\"end\":{\"line\":0,\"character\":3}},\"newText\":\"invalid\"}]";
  assert(json_parse(&v, split_byte, strlen(split_byte)));
  assert(!lsp_apply_edits(&e, &v, 0, uri, 1, &version, 1));
  assert(!strcmp(buffer_line(&e.document.buf, 0), original));
  json_free(&v);
  dispatch_key(&e, META('z'));
  assert(!strcmp(buffer_line(&e.document.buf, 0), "è😀 word è😀 last"));
  free(uri);
  dispatch_shutdown(&e);
}
int main(int argc, char **argv) {
  assert(argc == 2);
  precise_positions();
  dense_edits();
  unicode_edits();
  editor e = {.document.filename = argv[1], .rows = 24, .cols = 80};
  assert(buffer_load_text(&e.document.buf, "print\nsecond") == 0);
  dispatch_init(&e);
  assert(test_shared_split(&e, LAYOUT_VERTICAL));
  char *target = malloc(strlen(argv[1]) + 8);
  assert(target);
  sprintf(target, "%s.other", argv[1]);
  assert(file_write(target, "print\n") == 0);
  char *a = lsp_uri(argv[1]), *b = lsp_uri(target);
  assert(a && b);
  lsp_edit_version versions[] = {{a, e.document.change_id}};
  json_buf valid = {0};
  json_add(&valid, "{\"changes\":{");
  edits(&valid, a, 5, "renamed");
  json_add(&valid, ",");
  edits(&valid, b, 5, "renamed");
  json_add(&valid, "}}");
  int tabs = e.tab_count;
  // Every planner allocation failure must preserve *all* text and views,
  // including failures in the second file after the first was prepared.
  for (int i = 0; i < 4; i++) {
    buffer_test_fail_after(i);
    assert(!apply(&e, &valid, versions, 1));
    buffer_test_fail_after(-1);
    unchanged(&e, tabs);
    assert(!documents_find(&e, target));
  }
  undo_test_io_failure(1);
  assert(!apply(&e, &valid, versions, 1));
  undo_test_io_failure(0);
  unchanged(&e, tabs);
  versions[0].generation++;
  assert(!apply(&e, &valid, versions, 1));
  versions[0].generation--;
  unchanged(&e, tabs);
  json_buf bad = {0};
  json_add(&bad, "{\"changes\":{");
  edits(&bad, a, 5, "first");
  json_add(&bad, ",");
  edits(&bad, b, 999, "invalid range");
  json_add(&bad, "}}");
  assert(!apply(&e, &bad, versions, 1));
  unchanged(&e, tabs);
  json_buf_free(&bad);
  json_add(&bad, "{\"changes\":{");
  json_quote(&bad, a);
  json_add(&bad, ":[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":"
                 "{\"line\":0,\"character\":4}},\"newText\":\"one\"},{"
                 "\"range\":{\"start\":{\"line\":0,\"character\":2},\"end\":{"
                 "\"line\":0,\"character\":5}},\"newText\":\"two\"}]}}");
  assert(!apply(&e, &bad, versions, 1));
  unchanged(&e, tabs);
  json_buf_free(&bad);
  json_add(&bad, "{\"documentChanges\":[{\"kind\":\"delete\",\"uri\":");
  json_quote(&bad, b);
  json_add(&bad, "}]}");
  assert(!apply(&e, &bad, versions, 1));
  unchanged(&e, tabs);
  assert(apply(&e, &valid, versions, 1));
  document *other = documents_find(&e, target);
  assert(other && other->views == 1 && other->dirty);
  assert(e.tab_count == tabs + 1 && e.document.dirty);
  assert(!strcmp(buffer_line(&e.document.buf, 0), "renamed"));
  assert(!strcmp(buffer_line(&other->buf, 0), "renamed"));
  char *disk = file_read(target);
  assert(disk && !strcmp(disk, "print\n"));
  free(disk);
  dispatch_key(&e, CTRL('z'));
  assert(!strcmp(buffer_line(&e.document.buf, 0), "print"));
  assert(!e.document.dirty && other->dirty);
  dispatch_key(&e, CTRL('p'));
  assert(e.view->doc == other);
  dispatch_key(&e, CTRL('z'));
  assert(!strcmp(buffer_line(&other->buf, 0), "print") && !other->dirty);
  dispatch_key(&e, META('z'));
  assert(!strcmp(buffer_line(&other->buf, 0), "renamed") && other->dirty);
  dispatch_key(&e, CTRL('x'));
  dispatch_key(&e, 'k');
  assert(e.confirmation);
  dispatch_key(&e, 'y');
  assert(!documents_find(&e, target) && e.tab_count == tabs);
  json_buf_free(&bad);
  json_buf_free(&valid);
  free(a);
  free(b);
  free(target);
  dispatch_shutdown(&e);
  return 0;
}
