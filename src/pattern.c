#include "pattern.h"
#include "utf8.h"
#include <limits.h>
#include <string.h>
#if (defined(__x86_64__) || defined(__i386__)) &&                              \
    (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define SIMD_X86 1
#elif defined(__aarch64__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define SIMD_NEON 1
#endif

enum { CHAR, ANY, CLASS, BOL, EOL, SPLIT, JUMP, MATCH };
typedef struct fragment {
  int start, head, tail;
} fragment;
typedef struct parser {
  search_pattern *p;
  int at, failed, depth, links[PATTERN_STATES * 2];
} parser;
static int node(parser *r, int kind, int out, int alt) {
  if (r->p->count == PATTERN_STATES) {
    r->failed = 1;
    return 0;
  }
  int n = r->p->count++;
  r->p->nodes[n] = (pattern_node){.kind = kind, .out = out, .alt = alt};
  return n;
}
static fragment single(parser *r, int n, int field) {
  int link = n * 2 + field;
  r->links[link] = -1;
  return (fragment){n, link, link};
}
static void patch(parser *r, fragment f, int target) {
  for (int link = f.head; link >= 0; link = r->links[link]) {
    pattern_node *n = &r->p->nodes[link / 2];
    if (link % 2)
      n->alt = target;
    else
      n->out = target;
  }
}
static fragment merge(parser *r, fragment a, fragment b) {
  r->links[a.tail] = b.head;
  a.tail = b.tail;
  return a;
}
static fragment expression(parser *r);
static fragment atom(parser *r) {
  char c = r->p->text[r->at];
  if (c == '(') {
    r->at++;
    if (++r->depth > 64) {
      r->failed = 1;
      return single(r, node(r, JUMP, -1, 0), 0);
    }
    fragment f = expression(r);
    if (r->p->text[r->at] != ')')
      r->failed = 1;
    else
      r->at++;
    r->depth--;
    return f;
  }
  int n = node(r,
               c == '.'   ? ANY
               : c == '^' ? BOL
               : c == '$' ? EOL
               : c == '[' ? CLASS
                          : CHAR,
               -1, 0);
  pattern_node *s = &r->p->nodes[n];
  if (c == '[') {
    int start = ++r->at;
    if (r->p->text[r->at] == '^')
      r->at++;
    if (r->p->text[r->at] == ']')
      r->at++;
    while (r->p->text[r->at] && r->p->text[r->at] != ']') {
      if (r->p->text[r->at] == '\\') {
        r->at++;
        if (!r->p->text[r->at])
          break;
      }
      int cp;
      r->at += utf8_decode(r->p->text + r->at, &cp);
    }
    if (r->p->text[r->at] != ']')
      r->failed = 1;
    s->offset = start;
    s->length = r->at - start;
    if (!r->failed)
      r->at++;
  } else if (c == '\\') {
    r->at++;
    char e = r->p->text[r->at];
    if (!e || (e >= '1' && e <= '9')) {
      r->failed = 1;
      return single(r, n, 0);
    }
    if (strchr("dDwWsS", e)) {
      s->kind = CLASS;
      s->offset = r->at - 1;
      s->length = 2;
      r->at++;
    } else {
      s->value = e == 't' ? '\t' : e == 'r' ? '\r' : e == 'n' ? '\n' : 0;
      int cp, len = utf8_decode(r->p->text + r->at, &cp);
      if (!s->value)
        s->value = cp;
      r->at += len;
    }
  } else {
    int cp;
    r->at += utf8_decode(r->p->text + r->at, &cp);
    s->value = cp;
  }
  return single(r, n, 0);
}
static fragment sequence(parser *r) {
  fragment f = single(r, node(r, JUMP, -1, 0), 0);
  while (!r->failed && r->p->text[r->at] && r->p->text[r->at] != ')' &&
         r->p->text[r->at] != '|') {
    char c = r->p->text[r->at];
    if (strchr("*+?{}", c)) {
      r->failed = 1;
      break;
    }
    fragment a = atom(r);
    c = r->p->text[r->at];
    if (c == '*' || c == '+' || c == '?') {
      r->at++;
      int n = node(r, SPLIT, a.start, -1);
      fragment tail = single(r, n, 1);
      if (c == '?') {
        a = merge(r, a, tail);
        a.start = n;
      } else {
        patch(r, a, n);
        a = (fragment){c == '*' ? n : a.start, tail.head, tail.tail};
      }
    }
    patch(r, f, a.start);
    f = (fragment){f.start, a.head, a.tail};
  }
  return f;
}
static fragment expression(parser *r) {
  fragment a = sequence(r);
  while (!r->failed && r->p->text[r->at] == '|') {
    r->at++;
    fragment b = sequence(r);
    int n = node(r, SPLIT, a.start, b.start);
    a = merge(r, a, b);
    a.start = n;
  }
  return a;
}
// Select two discriminating bytes. Runtime dispatch keeps the binary usable on
// older CPUs and over SSH; no terminal protocol or global -march is required.
static void prepare_literal(search_pattern *p) {
  int n = p->literal_length;
  for (int i = 1, matched = 0; i < n; i++) {
    while (matched && p->literal[i] != p->literal[matched])
      matched = p->fallback[matched - 1];
    if (p->literal[i] == p->literal[matched])
      matched++;
    p->fallback[i] = matched;
  }
  p->first_byte = 0;
  p->second_byte = n ? n - 1 : 0;
  for (int i = 1; i < n; i++)
    if (p->literal[i] != p->literal[0]) {
      p->second_byte = i;
      break;
    }
#ifdef SIMD_X86
  __builtin_cpu_init();
  p->simd = __builtin_cpu_supports("avx2")   ? 2
            : __builtin_cpu_supports("sse2") ? 1
                                             : 0;
#elif defined(SIMD_NEON)
  p->simd = 3;
#endif
}
// Dense false candidates can defeat a byte-pair prefilter. Switch to a
// linear-time matcher after a bounded number of failed verifications.
static int literal_kmp(const search_pattern *p, const char *s, int size,
                       int from, const atomic_int *cancel) {
  for (int i = from, matched = 0; i < size; i++) {
    if (((i - from) & 4095) == 0 && cancel && atomic_load(cancel))
      return -2;
    while (matched && s[i] != p->literal[matched])
      matched = p->fallback[matched - 1];
    if (s[i] == p->literal[matched])
      matched++;
    if (matched == p->literal_length)
      return i - matched + 1;
  }
  return -1;
}
// The portable path uses the C library's vectorized memchr where available.
// Bounded blocks keep cancellation responsive even on a very long line.
static int literal_scalar(const search_pattern *p, const char *s, int size,
                          int from, const atomic_int *cancel) {
  int limit = size - p->literal_length, rejected = 0;
  for (int i = from; i <= limit;) {
    if (cancel && atomic_load(cancel))
      return -2;
    int n = limit - i + 1;
    if (n > 4096)
      n = 4096;
    const char *hit =
        memchr(s + i + p->first_byte, (unsigned char)p->literal[p->first_byte],
               (size_t)n);
    if (!hit) {
      i += n;
      continue;
    }
    i = (int)(hit - s) - p->first_byte;
    if (s[i + p->second_byte] == p->literal[p->second_byte] &&
        !memcmp(s + i, p->literal, (size_t)p->literal_length))
      return i;
    i++;
    if (++rejected == 64)
      return literal_kmp(p, s, size, i, cancel);
  }
  return -1;
}
#ifdef SIMD_X86
#define VECTOR_SEARCH(NAME, WIDTH, V, LOAD, SET, EQ, AND, MASK, ATTR)          \
  ATTR static int NAME(const search_pattern *p, const char *s, int size,       \
                       int from, const atomic_int *cancel) {                   \
    int i = from, limit = size - p->literal_length, rejected = 0;              \
    V a = SET(p->literal[p->first_byte]), b = SET(p->literal[p->second_byte]); \
    while (limit - i >= WIDTH - 1) {                                           \
      if (((i - from) & 4095) == 0 && cancel && atomic_load(cancel))           \
        return -2;                                                             \
      V x = LOAD((const V *)(s + i + p->first_byte));                          \
      V y = LOAD((const V *)(s + i + p->second_byte));                         \
      unsigned mask = (unsigned)MASK(AND(EQ(x, a), EQ(y, b)));                 \
      while (mask) {                                                           \
        int at = i + __builtin_ctz(mask);                                      \
        if (!memcmp(s + at, p->literal, (size_t)p->literal_length))            \
          return at;                                                           \
        if (++rejected == 64)                                                  \
          return literal_kmp(p, s, size, at + 1, cancel);                      \
        mask &= mask - 1;                                                      \
      }                                                                        \
      i += WIDTH;                                                              \
    }                                                                          \
    return literal_scalar(p, s, size, i, cancel);                              \
  }
VECTOR_SEARCH(literal_sse2, 16, __m128i, _mm_loadu_si128, _mm_set1_epi8,
              _mm_cmpeq_epi8, _mm_and_si128, _mm_movemask_epi8,
              __attribute__((target("sse2"))))
VECTOR_SEARCH(literal_avx2, 32, __m256i, _mm256_loadu_si256, _mm256_set1_epi8,
              _mm256_cmpeq_epi8, _mm256_and_si256, _mm256_movemask_epi8,
              __attribute__((target("avx2"))))
#undef VECTOR_SEARCH
#endif
#ifdef SIMD_NEON
static int literal_neon(const search_pattern *p, const char *s, int size,
                        int from, const atomic_int *cancel) {
  int i = from, limit = size - p->literal_length, rejected = 0;
  uint8x16_t a = vdupq_n_u8((unsigned char)p->literal[p->first_byte]);
  uint8x16_t b = vdupq_n_u8((unsigned char)p->literal[p->second_byte]);
  while (limit - i >= 15) {
    if (((i - from) & 4095) == 0 && cancel && atomic_load(cancel))
      return -2;
    uint8x16_t mask = vandq_u8(
        vceqq_u8(vld1q_u8((const uint8_t *)s + i + p->first_byte), a),
        vceqq_u8(vld1q_u8((const uint8_t *)s + i + p->second_byte), b));
    if (vmaxvq_u8(mask)) {
      uint8_t candidates[16];
      vst1q_u8(candidates, mask);
      for (int k = 0; k < 16; k++)
        if (candidates[k]) {
          if (!memcmp(s + i + k, p->literal, (size_t)p->literal_length))
            return i + k;
          if (++rejected == 64)
            return literal_kmp(p, s, size, i + k + 1, cancel);
        }
    }
    i += 16;
  }
  return literal_scalar(p, s, size, i, cancel);
}
#endif
static int literal_find(const search_pattern *p, const char *s, int size,
                        int from, const atomic_int *cancel) {
  if (cancel && atomic_load(cancel))
    return -2;
  if (!p->literal_length)
    return -1;
#ifdef SIMD_X86
  // Short lines and one-byte patterns are cheaper through libc.
  if (p->literal_length > 1 && size - from >= 64) {
    if (p->simd == 2)
      return literal_avx2(p, s, size, from, cancel);
    if (p->simd == 1)
      return literal_sse2(p, s, size, from, cancel);
  }
#elif defined(SIMD_NEON)
  if (p->simd == 3 && p->literal_length > 1 && size - from >= 64)
    return literal_neon(p, s, size, from, cancel);
#endif
  return literal_scalar(p, s, size, from, cancel);
}
static int class_hit(const search_pattern *p, const pattern_node *n, int cp);
// ASCII membership is precomputed. Non-ASCII lead bytes remain candidates so
// the UTF-8 matcher, including its malformed-byte semantics, stays
// authoritative.
static int class_scalar(const search_pattern *p, const char *s, int size,
                        int from, const atomic_int *cancel) {
  for (int i = from; i < size; i++) {
    if (((i - from) & 4095) == 0 && cancel && atomic_load(cancel))
      return -2;
    unsigned c = (unsigned char)s[i];
    if (c >= 128 || (p->byte_class[c & 15] & (1u << (c >> 4))))
      return i;
  }
  return -1;
}
#ifdef SIMD_X86
__attribute__((target("avx2"))) static int
class_avx2(const search_pattern *p, const char *s, int size, int from,
           const atomic_int *cancel) {
  const unsigned char powers[16] = {1, 2, 4, 8, 16, 32, 64, 128,
                                    0, 0, 0, 0, 0,  0,  0,  0};
  __m256i low = _mm256_broadcastsi128_si256(
      _mm_loadu_si128((const __m128i *)p->byte_class));
  __m256i high =
      _mm256_broadcastsi128_si256(_mm_loadu_si128((const __m128i *)powers));
  __m256i zero = _mm256_setzero_si256(), nibble = _mm256_set1_epi8(15);
  int i = from;
  for (; size - i >= 32; i += 32) {
    if (((i - from) & 4095) == 0 && cancel && atomic_load(cancel))
      return -2;
    __m256i x = _mm256_loadu_si256((const __m256i *)(s + i));
    __m256i a = _mm256_shuffle_epi8(low, _mm256_and_si256(x, nibble));
    __m256i b = _mm256_shuffle_epi8(
        high, _mm256_and_si256(_mm256_srli_epi16(x, 4), nibble));
    __m256i members = _mm256_and_si256(a, b);
    __m256i mask =
        _mm256_or_si256(_mm256_cmpgt_epi8(zero, x),
                        _mm256_xor_si256(_mm256_cmpeq_epi8(members, zero),
                                         _mm256_set1_epi8(-1)));
    unsigned bits = (unsigned)_mm256_movemask_epi8(mask);
    if (bits)
      return i + __builtin_ctz(bits);
  }
  return class_scalar(p, s, size, i, cancel);
}
#endif
#ifdef SIMD_NEON
static int class_neon(const search_pattern *p, const char *s, int size,
                      int from, const atomic_int *cancel) {
  const uint8_t powers[16] = {1, 2, 4, 8, 16, 32, 64, 128,
                              0, 0, 0, 0, 0,  0,  0,  0};
  uint8x16_t low = vld1q_u8(p->byte_class), high = vld1q_u8(powers);
  int i = from;
  for (; size - i >= 16; i += 16) {
    if (((i - from) & 4095) == 0 && cancel && atomic_load(cancel))
      return -2;
    uint8x16_t x = vld1q_u8((const uint8_t *)s + i);
    uint8x16_t members = vandq_u8(vqtbl1q_u8(low, vandq_u8(x, vdupq_n_u8(15))),
                                  vqtbl1q_u8(high, vshrq_n_u8(x, 4)));
    uint8x16_t mask = vorrq_u8(vcgeq_u8(x, vdupq_n_u8(128)),
                               vcgtq_u8(members, vdupq_n_u8(0)));
    if (vmaxvq_u8(mask)) {
      uint8_t candidates[16];
      vst1q_u8(candidates, mask);
      for (int k = 0; k < 16; k++)
        if (candidates[k])
          return i + k;
    }
  }
  return class_scalar(p, s, size, i, cancel);
}
#endif
static int class_find(const search_pattern *p, const char *s, int size,
                      int from, const atomic_int *cancel) {
#ifdef SIMD_X86
  if (p->simd == 2)
    return class_avx2(p, s, size, from, cancel);
#elif defined(SIMD_NEON)
  if (p->simd == 3)
    return class_neon(p, s, size, from, cancel);
#endif
  return class_scalar(p, s, size, from, cancel);
}
int pattern_compile(search_pattern *p, const char *text, int regex) {
  memset(p, 0, sizeof *p);
  p->first_class = -1;
  size_t len = strlen(text);
  if (len >= PATTERN_MAX)
    return 0;
  memcpy(p->text, text, len + 1);
  p->length = (int)len;
  p->regex = regex;
  if (!regex) {
    memcpy(p->literal, text, len + 1);
    p->literal_length = p->length;
    p->literal_only = 1;
    prepare_literal(p);
    return 1;
  }
  parser r = {.p = p};
  fragment f = expression(&r);
  if (r.failed || p->text[r.at])
    return 0;
  int end = node(&r, MATCH, 0, 0);
  patch(&r, f, end);
  p->start = f.start;
  for (int i = 0; i < p->count; i++)
    if (p->nodes[i].kind == CLASS)
      for (int c = 0; c < 128; c++)
        if (class_hit(p, &p->nodes[i], c))
          p->nodes[i].ascii[c / 64] |= UINT64_C(1) << (c % 64);
  // A mandatory ASCII prefix rejects most lines before entering the NFA.
  // Stop at branches/quantifiers/classes: an optional prefix is never safe.
  int at = p->start, anchored = 0;
  for (int steps = 0; steps < p->count; steps++) {
    const pattern_node *n = &p->nodes[at];
    if (n->kind == CHAR && n->value > 0 && n->value < 128)
      p->literal[p->literal_length++] = (char)n->value;
    else if (n->kind == BOL)
      anchored = 1;
    else if (n->kind != JUMP) {
      p->literal_only = n->kind == MATCH && !anchored && p->literal_length > 0;
      if (n->kind == CLASS && !p->literal_length) {
        int members = 0;
        for (int c = 0; c < 128; c++)
          if (n->ascii[c / 64] & (UINT64_C(1) << (c % 64))) {
            p->byte_class[c & 15] |= (unsigned char)(1u << (c >> 4));
            members++;
          }
        if (members <= 64)
          p->first_class = at;
      }
      break;
    }
    at = n->out;
  }
  prepare_literal(p);
  return !r.failed;
}
static int escape(char c, int cp) {
  int digit = cp >= '0' && cp <= '9';
  int word = digit || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
             cp == '_' || cp >= 128;
  int space = cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' ||
              cp == '\f' || cp == '\v';
  switch (c) {
  case 'd':
    return digit;
  case 'D':
    return !digit;
  case 'w':
    return word;
  case 'W':
    return !word;
  case 's':
    return space;
  case 'S':
    return !space;
  default:
    return -1;
  }
}
static int class_cp(const char *s, int *at, int end, int cp, int *literal) {
  if (*at < end && s[*at] == '\\') {
    (*at)++;
    if (*at >= end)
      return 0;
    int hit = escape(s[*at], cp);
    if (hit >= 0) {
      (*at)++;
      *literal = -1;
      return hit;
    }
    char c = s[*at];
    if (c == 't' || c == 'r' || c == 'n') {
      (*at)++;
      *literal = c == 't' ? '\t' : c == 'r' ? '\r' : '\n';
      return cp == *literal;
    }
  }
  *at += utf8_decode(s + *at, literal);
  return cp == *literal;
}
static int class_hit(const search_pattern *p, const pattern_node *n, int cp) {
  const char *s = p->text;
  int at = n->offset, end = at + n->length;
  if (s[at] == '\\' && n->length == 2)
    return escape(s[at + 1], cp);
  int negate = s[at] == '^', hit = 0;
  at += negate;
  while (at < end) {
    int lo;
    int one = class_cp(s, &at, end, cp, &lo);
    if (lo >= 0 && at + 1 < end && s[at] == '-') {
      at++;
      int hi;
      class_cp(s, &at, end, cp, &hi);
      if (hi >= 0)
        one = cp >= lo && cp <= hi;
    }
    hit |= one;
  }
  return negate ? !hit : hit;
}
// Epsilon closure stores only the earliest start at each state. A bounded
// state set replaces recursive backtracking even for nested repetitions.
static void closure(const search_pattern *p, int *states, int n, int start,
                    int pos, int size) {
  int stack[PATTERN_STATES * 2], origins[PATTERN_STATES * 2], count = 0;
  stack[count] = n;
  origins[count++] = start;
  while (count) {
    n = stack[--count];
    start = origins[count];
    if (states[n] <= start)
      continue;
    states[n] = start;
    const pattern_node *s = &p->nodes[n];
    if (s->kind == SPLIT) {
      stack[count] = s->alt;
      origins[count++] = start;
      stack[count] = s->out;
      origins[count++] = start;
    } else if (s->kind == JUMP || (s->kind == BOL && pos == 0) ||
               (s->kind == EOL && pos == size)) {
      stack[count] = s->out;
      origins[count++] = start;
    }
  }
}
int pattern_find(const search_pattern *p, const char *line, int size, int from,
                 int *length, const atomic_int *cancel) {
  if (from < 0 || from > size)
    return -1;
  if (p->literal_only) {
    int found = literal_find(p, line, size, from, cancel);
    if (found >= 0)
      *length = p->literal_length;
    return found;
  }
  int a[PATTERN_STATES], b[PATTERN_STATES], *current = a, *next = b;
  for (int i = 0; i < p->count; i++)
    current[i] = INT_MAX;
  int best = -1, end = 0;
  int active = 0;
  for (int x = from;;) {
    if (cancel && atomic_load(cancel))
      return -2;
    if (best < 0 && !active && (p->literal_length || p->first_class >= 0)) {
      x = p->literal_length ? literal_find(p, line, size, x, cancel)
                            : class_find(p, line, size, x, cancel);
      if (x < 0)
        return x;
    }
    if (best < 0 &&
        (!p->literal_length ||
         (p->literal_length <= size - x &&
          !memcmp(line + x, p->literal, (size_t)p->literal_length))))
      closure(p, current, p->start, x, x, size);
    int live = 0;
    for (int i = 0; i < p->count; i++)
      if (current[i] != INT_MAX) {
        if (p->nodes[i].kind == MATCH) {
          if (best < 0 || current[i] < best ||
              (current[i] == best && x > end)) {
            best = current[i];
            end = x;
          }
        }
      }
    for (int i = 0; i < p->count; i++)
      if (current[i] != INT_MAX && (best < 0 || current[i] <= best)) {
        int k = p->nodes[i].kind;
        if (k == CHAR || k == ANY || k == CLASS)
          live = 1;
      }
    if ((best >= 0 && !live) || x == size)
      break;
    int cp, bytes = utf8_decode(line + x, &cp);
    for (int i = 0; i < p->count; i++)
      next[i] = INT_MAX;
    active = 0;
    for (int i = 0; i < p->count; i++)
      if (current[i] != INT_MAX && (best < 0 || current[i] <= best)) {
        const pattern_node *n = &p->nodes[i];
        if ((n->kind == CHAR && cp == n->value) || n->kind == ANY ||
            (n->kind == CLASS &&
             (cp < 128 ? !!(n->ascii[cp / 64] & (UINT64_C(1) << (cp % 64)))
                       : class_hit(p, n, cp)))) {
          closure(p, next, n->out, current[i], x + bytes, size);
          active = 1;
        }
      }
    int *swap = current;
    current = next;
    next = swap;
    x += bytes;
  }
  if (best >= 0)
    *length = end - best;
  return best;
}
