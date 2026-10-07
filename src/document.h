#ifndef DOCUMENT_H
#define DOCUMENT_H

#include "buffer.h"
#include "diff.h"

typedef struct document {
  buffer buf;
  struct undo_history *history;
  const char *filename;
  int dirty;
  int views;
  int allocated;
  int owns_filename;
  int readonly;
  char *label;
  diff_line_kind *diff_lines;
} document;

document *document_open(const char *path);
document *document_preview(const char *title, const char *text);
const char *document_name(const document *doc);
int document_matches(const document *doc, const char *path);
void document_retain(document *doc);
void document_release(document *doc);

#endif
