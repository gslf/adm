#ifndef DOCUMENT_H
#define DOCUMENT_H

#include "buffer.h"

typedef struct document {
  buffer buf;
  const char *filename;
  int dirty;
  int views;
  int allocated;
} document;

document *document_open(const char *path);
int document_matches(const document *doc, const char *path);
void document_retain(document *doc);
void document_release(document *doc);

#endif
