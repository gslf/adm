#include "pattern.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <regex.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
static void match(const char *query, const char *text, int regex, int expected,
                  int size) {
  search_pattern p;
  assert(pattern_compile(&p, query, regex));
  int length = -99;
  int found = pattern_find(&p, text, (int)strlen(text), 0, &length, NULL);
  if (found != expected || (found >= 0 && length != size))
    fprintf(stderr, "%s in %s: %d:%d wanted %d:%d\n", query, text, found,
            length, expected, size);
  assert(found == expected);
  if (found >= 0)
    assert(length == size);
}
static int naive(const char *s, int n, const char *q, int m, int from) {
  for (int i = from; i <= n - m; i++)
    if (!memcmp(s + i, q, (size_t)m))
      return i;
  return -1;
}
static void literal_regressions(void) {
  unsigned seed = 19;
  for (int trial = 0; trial < 4000; trial++) {
    char text[1100], query[256];
    int n = trial % 1025, m = trial % 255 + 1;
    for (int i = 0; i < n; i++) {
      seed = seed * 1664525u + 1013904223u;
      text[i] = (char)(1 + (seed >> 16) % (trial % 3 ? 255 : 2));
    }
    text[n] = 0;
    for (int i = 0; i < m; i++)
      query[i] = (char)(1 + i % 7);
    if (trial % 2 && m <= n)
      memcpy(query, text + (n - m) / 2, (size_t)m);
    query[m] = 0;
    search_pattern p;
    assert(pattern_compile(&p, query, 0));
    int maximum = p.simd;
    for (int mode = 0; mode <= maximum; mode++) {
      p.simd = mode;
      int length, from = n ? trial % (n + 1) : 0;
      int expected = naive(text, n, query, m, from);
      assert(pattern_find(&p, text, n, from, &length, NULL) == expected);
      if (expected >= 0)
        assert(length == m);
    }
  }
  char repetitive[8193], query[255];
  for (int i = 0; i < 8192; i++)
    repetitive[i] = "ab"[i % 2];
  repetitive[8192] = 0;
  for (int i = 0; i < 253; i++)
    query[i] = "ab"[i % 2];
  query[253] = 'c';
  query[254] = 0;
  search_pattern pattern;
  assert(pattern_compile(&pattern, query, 0));
  int maximum = pattern.simd;
  for (int mode = 0; mode <= maximum; mode++) {
    pattern.simd = mode;
    int length;
    assert(pattern_find(&pattern, repetitive, 8192, 0, &length, NULL) == -1);
    memcpy(repetitive + 7000, query, 254);
    assert(pattern_find(&pattern, repetitive, 8192, 0, &length, NULL) == 7000);
    for (int i = 7000; i < 7254; i++)
      repetitive[i] = "ab"[i % 2];
  }
#ifndef _WIN32
  // Put the line's last byte at a protected page boundary. SIMD must not
  // overread, even for offset probes and short tails.
  long page = sysconf(_SC_PAGESIZE);
  char *pages = mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(pages != MAP_FAILED);
  assert(mprotect(pages + page, (size_t)page, PROT_NONE) == 0);
  for (int n = 1; n < 513; n++) {
    char *s = pages + page - n;
    memset(s, 'a', (size_t)n);
    search_pattern p;
    assert(pattern_compile(&p, "aaab", 0));
    int length;
    assert(pattern_find(&p, s, n, 0, &length, NULL) == -1);
    assert(pattern_compile(&p, "[0-9]+", 1));
    assert(pattern_find(&p, s, n, 0, &length, NULL) == -1);
    s[n - 1] = '7';
    assert(pattern_find(&p, s, n, 0, &length, NULL) == n - 1 && length == 1);
    assert(pattern_compile(&p, "aaab", 0));
    s[n - 1] = 'b';
    assert(pattern_find(&p, s, n, 0, &length, NULL) == (n >= 4 ? n - 4 : -1));
  }
  munmap(pages, (size_t)page * 2);
#endif
}
static void regex_filters(void) {
  const char *queries[] = {"[0-9]+", "[A-F]+z", "[^abc]d", "[é界]+",
                           "\\d+x",  "\\w+",    "\\s+b",   "abc.*d",
                           "^abc",   "abc$",    "abc?",    "ab+c"};
  unsigned seed = 73;
  for (int trial = 0; trial < 1200; trial++) {
    char text[514];
    int n = trial % 513;
    for (int i = 0; i < n; i++) {
      seed = seed * 1664525u + 1013904223u;
      text[i] = "aAbBcz 19\xc3\xa9\xe7\x95\x8c\xe9\xf0"[(seed >> 16) % 16];
    }
    text[n] = 0;
    search_pattern p;
    assert(pattern_compile(
        &p, queries[trial % (sizeof queries / sizeof *queries)], 1));
    search_pattern reference = p;
    reference.first_class = -1;
    reference.literal_length = reference.literal_only = 0;
    int from = n ? trial % n : 0, expected_length;
    int expected =
        pattern_find(&reference, text, n, from, &expected_length, NULL);
    int maximum = p.simd;
    for (int mode = 0; mode <= maximum; mode++) {
      p.simd = mode;
      int length;
      assert(pattern_find(&p, text, n, from, &length, NULL) == expected);
      if (expected >= 0)
        assert(length == expected_length);
    }
  }
}
int main(int argc, char **argv) {
  literal_regressions();
  regex_filters();
  (void)argc;
  (void)argv;
  match("a.c", "--a.c", 0, 2, 3);
  match("a.c", "--abc", 0, -1, 0);
  match("a.c", "--abc", 1, 2, 3);
  match("a+", "baaaac", 1, 1, 4);
  match("(ab|a)+c", "zababc", 1, 1, 5);
  match("(a|aa)*b", "aaaaac", 1, -1, 0);
  match("^a.*z$", "aZZz", 1, 0, 4);
  match("^a", "ba", 1, -1, 0);
  match("$", "abc", 1, 3, 0);
  match("(ab)?c", "abc", 1, 0, 3);
  match("[a-z]+\\d+", "ZZword123!", 1, 2, 7);
  match("[^0-9]+", "12é界!3", 1, 2, 6);
  match("[]-]+", "xy]-!", 1, 2, 2);
  match("\\w+", "界_a!", 1, 0, 5);
  match("a|aa", "xaa", 1, 1, 2);
  match("(a*)*", "aaa", 1, 0, 3);
  match("^|$", "abc", 1, 0, 0);
  match("\\.", "x.y", 1, 1, 1);
  const char *bad[] = {"[", "a**", "(", "a{2}", "(?=a)", "\\1", "\\", "*a"};
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
    search_pattern p;
    assert(!pattern_compile(&p, bad[i], 1));
  }
  search_pattern p;
  assert(pattern_compile(&p, "(a|aa)*b", 1));
  char *huge = malloc(200001);
  assert(huge);
  memset(huge, 'a', 200000);
  huge[200000] = 0;
  int length;
  assert(pattern_find(&p, huge, 200000, 0, &length, NULL) == -1);
  atomic_int cancelled;
  atomic_init(&cancelled, 1);
  assert(pattern_find(&p, huge, 200000, 0, &length, &cancelled) == -2);
  free(huge);
#ifndef _WIN32
  const char *patterns[] = {
      "a*b",    "(ab|a)+c", "a|aa",    "(a*)*",  "[abc]+", "[^ab]*c",
      "^a?b*$", "ab?|bc",   "(a|b)*c", "a.*b|c", "abc.*d", "^abc.*d",
      "abc$",   "abc?d",    "(ab)c",   "abc+",   "a\\.b"};
  unsigned random = 7;
  for (int trial = 0; trial < 2000; trial++) {
    char text[30];
    for (int i = 0; i < 29; i++) {
      random = random * 1664525u + 1013904223u;
      text[i] = "abcd"[(random >> 16) % 4];
    }
    text[29] = 0;
    const char *query = patterns[trial % (sizeof patterns / sizeof *patterns)];
    regex_t reference;
    regmatch_t span;
    assert(regcomp(&reference, query, REG_EXTENDED) == 0);
    int expected =
        regexec(&reference, text, 1, &span, 0) == 0 ? (int)span.rm_so : -1;
    search_pattern pattern;
    assert(pattern_compile(&pattern, query, 1));
    int actual = pattern_find(&pattern, text, 29, 0, &length, NULL);
    assert(actual == expected);
    if (actual >= 0)
      assert(length == span.rm_eo - span.rm_so);
    regfree(&reference);
  }
#endif
  puts("Pattern regressions passed.");
  return 0;
}
