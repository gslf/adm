#include "json.h"
#include "language.h"
#include "lsp.h"
#include "path.h"
#include "shared_views.h"
#include "syntax.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static syntax_kind kind(const language *l, const char *s, int at,
                        uint64_t *state) {
  syntax_span spans[64];
  size_t n = l->highlight(s, state, spans, 64);
  for (size_t i = 0; i < n; i++)
    if (at >= spans[i].start && at < spans[i].end)
      return spans[i].kind;
  return SYNTAX_TEXT;
}
int main(void) {
  assert(language_count() == 5);
  char *script = language_javascript()->server_script("/a space/cli.mjs");
  assert(script && !strcmp(script, "/a space/cli.mjs"));
  free(script);
  assert(language_for_filename("dir.py/file.unknown") == NULL);
  assert(language_for_filename("/space/file.c") == language_c());
  assert(language_for_filename("file.C") == language_cpp());
  assert(language_for_filename("foo.hpp") == language_cpp());
  assert(language_for_filename("foo.mjs") == language_javascript());
  assert(language_for_filename("foo.rs") == language_rust());
  assert(language_for_filename("foo.pyi") == language_python());
  uint64_t state = 0;
  assert(kind(language_c(), "int main() { return 42; } // hi", 0, &state) ==
         SYNTAX_TYPE);
  assert(kind(language_c(), "return 42;", 0, &state) == SYNTAX_KEYWORD);
  assert(kind(language_c(), "return 42;", 7, &state) == SYNTAX_NUMBER);
  assert(kind(language_c(), "\"// string\"", 3, &state) == SYNTAX_STRING);
  assert(kind(language_c(), "/* multi", 0, &state) == SYNTAX_COMMENT && state);
  assert(kind(language_c(), "continued */ return", 0, &state) ==
             SYNTAX_COMMENT &&
         !state);
  state = 0;
  assert(kind(language_rust(), "/* outer /* inner", 0, &state) ==
         SYNTAX_COMMENT);
  assert(kind(language_rust(), "*/ still", 3, &state) == SYNTAX_COMMENT &&
         state);
  assert(kind(language_rust(), "*/ fn", 3, &state) == SYNTAX_KEYWORD && !state);
  state = 0;
  assert(kind(language_rust(), "r##\"abc", 1, &state) == SYNTAX_STRING &&
         state);
  assert(kind(language_rust(), "\"# unfinished", 3, &state) == SYNTAX_STRING &&
         state);
  assert(kind(language_rust(), "\"## fn", 4, &state) == SYNTAX_KEYWORD &&
         !state);
  state = 0;
  assert(kind(language_rust(), "'static fn", 8, &state) == SYNTAX_KEYWORD);
  state = 0;
  assert(kind(language_python(), "\"\"\"first", 0, &state) == SYNTAX_STRING &&
         state);
  assert(kind(language_python(), "# inside\"\"\" def", 0, &state) ==
             SYNTAX_STRING &&
         !state);
  state = 0;
  assert(kind(language_javascript(), "`template", 0, &state) == SYNTAX_STRING &&
         state);
  assert(kind(language_javascript(), "end` const", 5, &state) ==
             SYNTAX_KEYWORD &&
         !state);
  // Prefix state invalidation, shared storage and structural edits.
  editor e = {.document.filename = "fixture.c", .rows = 24, .cols = 80};
  assert(buffer_load_text(&e.document.buf,
                          "/* comment\ninside\n*/ int value;\nreturn value;") ==
         0);
  dispatch_init(&e);
  syntax_prepare(&e.document, 4);
  syntax_span spans[64];
  assert(syntax_line(&e.document, 1, spans, 64) > 0 &&
         spans[0].kind == SYNTAX_COMMENT);
  assert(buffer_replace_range(&e.document.buf, 0, 0, 2, "//") == 0);
  dispatch_change(&e);
  syntax_prepare(&e.document, 4);
  assert(syntax_line(&e.document, 1, spans, 64) == 0);
  dispatch_key(&e, CTRL('z'));
  syntax_prepare(&e.document, 4);
  assert(syntax_line(&e.document, 1, spans, 64) > 0 &&
         spans[0].kind == SYNTAX_COMMENT);
  assert(buffer_insert_newline(&e.document.buf, 0, 0) == 0);
  dispatch_change(&e);
  syntax_prepare(&e.document, 5);
  assert(syntax_line(&e.document, 2, spans, 64) > 0 &&
         spans[0].kind == SYNTAX_COMMENT);
  dispatch_shutdown(&e);
  assert(!e.document.syntax);
  const char *text = "A\xc3\xa8\xf0\x9f\x98\x80Z";
  assert(lsp_position_units(text, 7, 0) == 4 &&
         lsp_position_units(text, 7, 1) == 7);
  assert(lsp_position_bytes(text, 4, 0) == 7 &&
         lsp_position_bytes(text, 3, 0) == 3);
  assert(lsp_position_bytes(text, 6, 1) == 3);
  char *uri = lsp_uri("a space \xc3\xa8 #%.c");
  assert(uri && strstr(uri, "%20") && strstr(uri, "%23%25"));
  char *path = lsp_uri_path(uri),
       *absolute = path_absolute("a space \xc3\xa8 #%.c");
  assert(path && absolute && !strcmp(path, absolute));
  free(uri);
  free(path);
  free(absolute);
  assert(!lsp_uri_path("file:///bad%00name") &&
         !lsp_uri_path("https://example.org/a"));
  json_value v = {0};
  const char truncated[] = {'"', (char)0xf0, (char)0x9f};
  assert(!json_parse(&v, truncated, sizeof truncated));
  const char *j = "{\"a\":[1,{\"s\":\"\\u00e8\\ud83d\\ude00\"}],\"b\":true}";
  assert(json_parse(&v, j, strlen(j)));
  size_t a = json_get(&v, 0, "a"), first = json_first(&v, a);
  int64_t n;
  assert(json_int(&v, first, &n) && n == 1);
  char *s = json_string(&v, json_get(&v, first + 1, "s"));
  assert(s && !strcmp(s, "\xc3\xa8\xf0\x9f\x98\x80"));
  free(s);
  json_free(&v);
  const char *bad[] = {"{\"x\":}",      "[1,]",         "01",
                       "1e+",           "\"bad\\q\"",   "{",
                       "null trailing", "\"\xc0\xaf\"", "\"\xed\xa0\x80\""};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
    assert(!json_parse(&v, bad[i], strlen(bad[i])));
  assert(json_parse(&v, "\"\\ud800\"", 8));
  assert(!json_string(&v, 0));
  json_free(&v);
  puts("Language regressions passed.");
  return 0;
}
