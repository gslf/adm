#ifndef PATTERN_H
#define PATTERN_H
#include <stdatomic.h>
#include <stdint.h>
#define PATTERN_MAX 256
#define PATTERN_STATES 768

typedef struct pattern_node {
  int kind, out, alt, value, offset, length;
  uint64_t ascii[2];
} pattern_node;
typedef struct search_pattern {
  char text[PATTERN_MAX];
  pattern_node nodes[PATTERN_STATES];
  int count, start, regex, length;
  char literal[PATTERN_MAX];
  int literal_length, first_byte, second_byte, simd, literal_only;
  int fallback[PATTERN_MAX];
  int first_class;
  unsigned char byte_class[16];
} search_pattern;
int pattern_compile(search_pattern *p, const char *text, int regex);
// Leftmost, longest, line-local match. Byte offsets. -1: none, -2: cancelled.
int pattern_find(const search_pattern *p, const char *line, int size, int from,
                 int *length, const atomic_int *cancel);
#endif
