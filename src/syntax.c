#include "theme.h"
#include "syntax.h"
#include "document.h"
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
#define LINE_LIMIT 65536
struct syntax_cache {
  const language *language;
  uint64_t *states;
  size_t capacity;
  int valid, resume, dirty_through;
};
void syntax_dispose(document *doc) {
  if (!doc || !doc->syntax)
    return;
  free(doc->syntax->states);
  free(doc->syntax);
  doc->syntax = NULL;
}
void syntax_invalidate(document *doc, const buffer_edit *edit) {
  if (!doc || !doc->syntax)
    return;
  struct syntax_cache *c = doc->syntax;
  int row = edit->row;
  if (edit->old_row != edit->new_row)
    c->resume = 0;
  else if (c->valid > c->resume)
    c->resume = c->valid;
  int through = edit->new_row + 1;
  if (through > c->dirty_through)
    c->dirty_through = through;
  if (row < c->valid)
    c->valid = row > 0 ? row : 0;
}
static struct syntax_cache *cache(document *doc) {
  const language *lang =
      !doc || doc->readonly ? NULL : language_for_filename(doc->filename);
  if (!lang)
    return NULL;
  if (doc->syntax && doc->syntax->language != lang)
    syntax_dispose(doc);
  if (!doc->syntax) {
    doc->syntax = calloc(1, sizeof *doc->syntax);
    if (doc->syntax)
      doc->syntax->language = lang;
  }
  return doc->syntax;
}
// Resumable prefix scan: at most two milliseconds and 256 KiB per frame.
// Large unknown prefixes show local colouring until multiline states catch up.
void syntax_prepare(document *doc, int through) {
  struct syntax_cache *c = cache(doc);
  if (!c)
    return;
  if (through > doc->buf.nlines)
    through = doc->buf.nlines;
  uint64_t deadline = adm_milliseconds() + 2;
  size_t bytes = 0;
  while (c->valid < through && bytes < 262144 &&
         adm_milliseconds() < deadline) {
    size_t needed = (size_t)c->valid + 2;
    if (needed > c->capacity) {
      size_t capacity = c->capacity ? c->capacity * 2 : 1024;
      if (capacity > SIZE_MAX / sizeof(uint64_t))
        return;
      uint64_t *more = realloc(c->states, capacity * sizeof *more);
      if (!more)
        return;
      c->states = more;
      c->capacity = capacity;
      if (!c->valid)
        c->states[0] = 0;
    }
    const char *line = buffer_line(&doc->buf, c->valid);
    size_t n = line ? strnlen(line, LINE_LIMIT + 1) : 0;
    uint64_t state = c->states[c->valid];
    if (n <= LINE_LIMIT && line)
      c->language->highlight(line, &state, NULL, 0);
    else
      state = 0;
    int next = c->valid + 1;
    if (next >= c->dirty_through && next < c->resume &&
        c->states[next] == state) {
      c->valid = c->resume;
      c->resume = c->dirty_through = 0;
      break;
    }
    c->states[++c->valid] = state;
    bytes += n;
  }
}
size_t syntax_line(document *doc, int row, syntax_span *out, size_t capacity) {
  struct syntax_cache *c = cache(doc);
  const char *line = doc ? buffer_line(&doc->buf, row) : NULL;
  if (!c || !line || strnlen(line, LINE_LIMIT + 1) > LINE_LIMIT)
    return 0;
  uint64_t state = row <= c->valid && c->states ? c->states[row] : 0;
  return c->language->highlight(line, &state, out, capacity);
}
const char *syntax_colour(syntax_kind kind) {
  static const theme_role roles[] = {THEME_NORMAL, THEME_KEYWORD, THEME_TYPE,
    THEME_STRING, THEME_COMMENT, THEME_NUMBER, THEME_DIRECTIVE, THEME_FUNCTION};
  return theme_colour(kind >= SYNTAX_TEXT && kind <= SYNTAX_FUNCTION ? roles[kind] : THEME_NORMAL);
}
