#include "diff.h"

#include <stdlib.h>
#include <string.h>

static diff_line_kind classify(const char *line, int *columns) {
  if (!strncmp(line, "diff ", 5)) {
    *columns = 0;
    return DIFF_METADATA;
  }
  if (!strncmp(line, "@@", 2)) {
    int marks = 2;
    while (line[marks] == '@')
      marks++;
    *columns = marks - 1;
    return DIFF_HUNK;
  }
  if (!*columns || !line[0] || line[0] == '\\')
    return DIFF_METADATA;

  int added = 0, removed = 0;
  for (int i = 0; i < *columns; i++) {
    if (line[i] != ' ' && line[i] != '+' && line[i] != '-')
      return DIFF_METADATA;
    added |= line[i] == '+';
    removed |= line[i] == '-';
  }
  return removed ? DIFF_REMOVED : added ? DIFF_ADDED : DIFF_CONTEXT;
}

diff_line_kind *diff_classify(const buffer *text) {
  diff_line_kind *lines = calloc(text->nlines ? text->nlines : 1, sizeof *lines);
  if (!lines)
    return NULL;
  // Classify once per immutable preview, so scrolling and shared views stay cheap.
  // Hunk prefixes distinguish code from file headers, including combined diffs.
  int row = 0, columns = 0;
  for (const block *b = text->head; b; b = b->next)
    for (int i = 0; i < b->count; i++)
      lines[row++] = classify(b->lines[i], &columns);
  return lines;
}
