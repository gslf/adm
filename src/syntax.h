#ifndef SYNTAX_H
#define SYNTAX_H
#include "buffer.h"
#include "language.h"
struct document;
void syntax_invalidate(struct document *doc, const buffer_edit *edit);
void syntax_dispose(struct document *doc);
void syntax_prepare(struct document *doc, int through);
size_t syntax_line(struct document *doc, int row, syntax_span *out,
                   size_t capacity);
const char *syntax_colour(syntax_kind kind);
#endif
