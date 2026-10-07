#include "view.h"
#include "utf8.h"

#include <string.h>

void view_bind_document(view *v, document *doc) {
  if (v->doc == doc)
    return;
  document_retain(doc);
  document_release(v->doc);
  rect area = v->area;
  *v = (view){.used = 1, .doc = doc, .area = area};
}

void view_dispose(view *v) {
  document_release(v->doc);
  *v = (view){0};
}

static int clamp(int n, int low, int high) {
  return n < low ? low : n > high ? high : n;
}

static void mark_column(view *v) {
  const char *line = buffer_line(&v->doc->buf, v->cy);
  v->sticky = line ? utf8_cols(line, v->cx) : 0;
}

static void move_position(int *row, int *col, const buffer_edit *edit) {
  if (*row < edit->row || (*row == edit->row && *col < edit->col))
    return;
  if (*row > edit->old_row ||
      (*row == edit->old_row && *col >= edit->old_col)) {
    if (*row == edit->old_row)
      *col += edit->new_col - edit->old_col;
    *row += edit->new_row - edit->old_row;
  } else {
    *row = edit->row;
    *col = edit->col;
  }
}

void view_rebase(view *v, const buffer_edit *edit) {
  int old_col = v->cx;
  move_position(&v->cy, &v->cx, edit);
  move_position(&v->sely, &v->selx, edit);
  int col = 0;
  move_position(&v->rowoff, &col, edit);
  if (v->cx != old_col)
    mark_column(v);
}

static void clamp_position(const buffer *b, int *row, int *col) {
  *row = clamp(*row, 0, b->nlines > 0 ? b->nlines - 1 : 0);
  const char *line = buffer_line(b, *row);
  *col = clamp(*col, 0, line ? (int)strlen(line) : 0);
  int offset = 0;
  while (line && offset < *col) {
    int next = grapheme_next(line, offset);
    if (next > *col)
      break;
    offset = next;
  }
  *col = offset;
}

void view_clamp(view *v) {
  if (!v->doc)
    return;
  const buffer *b = &v->doc->buf;
  int row = v->cy, col = v->cx;
  clamp_position(b, &v->cy, &v->cx);
  clamp_position(b, &v->sely, &v->selx);
  v->rowoff = clamp(v->rowoff, 0, b->nlines > 0 ? b->nlines - 1 : 0);
  if (v->cx == v->selx && v->cy == v->sely)
    v->sel_active = 0;
  // Unrelated edits must preserve the preferred column on short lines.
  if (v->cx != col || v->cy != row)
    mark_column(v);
}
