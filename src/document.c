#include "document.h"
#include "path.h"
#include "undo.h"
#include "syntax.h"
#include "lsp.h"

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

const char *document_name(const document *doc) {
  return !doc ? "[Empty]" : doc->label ? doc->label : doc->filename ? doc->filename : "[No Name]";
}

document *document_preview(const char *title, const char *text) {
  document *doc = calloc(1, sizeof *doc);
  if (!doc)
    return NULL;
  doc->label = malloc(strlen(title) + 1);
  if (!doc->label || buffer_load_text(&doc->buf, text) < 0) {
    buffer_free(&doc->buf);
    free(doc->label);
    free(doc);
    return NULL;
  }
  strcpy(doc->label, title);
  doc->diff_lines = diff_classify(&doc->buf);
  if (!doc->diff_lines) {
    buffer_free(&doc->buf);
    free(doc->label);
    free(doc);
    return NULL;
  }
  doc->allocated = doc->readonly = 1;
  return doc;
}

document *document_open(const char *path) {
  struct stat info;
  if (stat(path, &info) < 0)
    return NULL;
#ifdef _WIN32
  if ((info.st_mode & _S_IFMT) != _S_IFREG)
#else
  if (!S_ISREG(info.st_mode))
#endif
    return NULL;
  char *filename = path_absolute(path);
  document *doc = filename ? calloc(1, sizeof *doc) : NULL;
  if (!doc) {
    free(filename);
    return NULL;
  }
  if (load(&doc->buf, filename) < 0) {
    buffer_free(&doc->buf);
    free(filename);
    free(doc);
    return NULL;
  }
  doc->filename = filename;
  doc->allocated = 1;
  return doc;
}

int document_matches(const document *doc, const char *path) {
  if (!doc || !doc->filename)
    return 0;
  if (!strcmp(doc->filename, path))
    return 1;
#ifndef _WIN32
  struct stat left_info, right_info;
  if (stat(doc->filename, &left_info) == 0 && stat(path, &right_info) == 0 &&
      left_info.st_dev == right_info.st_dev && left_info.st_ino == right_info.st_ino)
    return 1;
#endif
  char *left = path_absolute(doc->filename), *right = path_absolute(path);
  int match = left && right &&
#ifdef _WIN32
      _stricmp(left, right) == 0;
#else
      strcmp(left, right) == 0;
#endif
  free(left);
  free(right);
  return match;
}

void document_retain(document *doc) {
  if (doc)
    doc->views++;
}

// Documents live exactly as long as their pane references, with no hidden list.
void document_release(document *doc) {
  if (!doc)
    return;
  assert(doc->views > 0);
  if (--doc->views > 0)
    return;
  lsp_detach(doc);
  syntax_dispose(doc);
  undo_clear(doc);
  buffer_free(&doc->buf);
  free(doc->diff_lines);
  doc->diff_lines = NULL;
  if (doc->allocated || doc->owns_filename)
    free((char *)doc->filename);
  if (doc->allocated) {
    free(doc->label);
    free(doc);
    return;
  }
  doc->filename = NULL;
  doc->owns_filename = 0;
  doc->dirty = 0;
}
