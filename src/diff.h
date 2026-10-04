#ifndef DIFF_H
#define DIFF_H

#include "buffer.h"

typedef enum {
  DIFF_METADATA, DIFF_HUNK, DIFF_CONTEXT, DIFF_ADDED, DIFF_REMOVED
} diff_line_kind;

diff_line_kind *diff_classify(const buffer *text);

#endif
