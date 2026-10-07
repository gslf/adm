#include "language.h"
#include <ctype.h>
#include <string.h>
#define MAX_LANGUAGES 32
static const language *registry[MAX_LANGUAGES];
static size_t count;
int language_register(const language *entry) {
  if (!entry || !entry->name || !entry->id || !entry->extensions ||
      !entry->highlight || !entry->server_key || !entry->server_env ||
      !entry->server_argv || !entry->server_argv[0])
    return 0;
  for (size_t i = 0; i < count; i++)
    if (registry[i] == entry)
      return 1;
  if (count == MAX_LANGUAGES)
    return 0;
  registry[count++] = entry;
  return 1;
}
void languages_init(void) {
  language_register(language_c());
  language_register(language_cpp());
  language_register(language_javascript());
  language_register(language_rust());
  language_register(language_python());
}
size_t language_count(void) {
  languages_init();
  return count;
}
const language *language_at(size_t i) {
  languages_init();
  return i < count ? registry[i] : NULL;
}
const language *language_for_filename(const char *path) {
  if (!path)
    return NULL;
  languages_init();
  const char *dot = NULL;
  for (const char *p = path; *p; p++) {
    if (*p == '/' || *p == '\\')
      dot = NULL;
    else if (*p == '.')
      dot = p;
  }
  if (!dot)
    return NULL;
  for (size_t i = 0; i < count; i++)
    for (const char *const *ext = registry[i]->extensions; *ext; ext++)
      if (!strcmp(dot, *ext))
        return registry[i];
  return NULL;
}
static int word(unsigned char c) { return isalnum(c) || c == '_' || c >= 128; }
static int member(const char *list, const char *s, size_t n) {
  if (!list)
    return 0;
  while (*list) {
    while (*list == ' ')
      list++;
    const char *end = strchr(list, ' ');
    size_t len = end ? (size_t)(end - list) : strlen(list);
    if (len == n && !memcmp(list, s, n))
      return 1;
    list += len;
  }
  return 0;
}
static void span(syntax_span *out, size_t capacity, size_t *n, int start,
                 int end, syntax_kind kind) {
  if (end > start && *n < capacity)
    out[(*n)++] = (syntax_span){start, end, kind};
}
// State: nested comment depth (low 16 bits), multiline quote (next byte),
// triple flag and Rust raw-string hash count. Shared by every language adapter.
size_t language_lex(const syntax_rules *r, const char *s, uint64_t *state,
                    syntax_span *out, size_t capacity) {
  unsigned depth = (unsigned)(*state & 65535),
           quote = (unsigned)(*state >> 16) & 255;
  int triple = (*state >> 24) & 1, raw = (*state >> 25) & 1;
  unsigned hashes = (unsigned)(*state >> 32);
  int i = 0;
  size_t n = 0;
  while (s[i]) {
    int start = i;
    if (depth) {
      while (s[i] && depth) {
        if (s[i] == '*' && s[i + 1] == '/') {
          depth--;
          i += 2;
        } else if (r->nested_comments && s[i] == '/' && s[i + 1] == '*') {
          if (depth < 65535)
            depth++;
          i += 2;
        } else
          i++;
      }
      span(out, capacity, &n, start, i, SYNTAX_COMMENT);
      continue;
    }
    if (quote) {
      while (s[i]) {
        if (!raw && s[i] == '\\' && s[i + 1]) {
          i += 2;
          continue;
        }
        if ((unsigned char)s[i] == quote) {
          if (triple && !(s[i + 1] == (char)quote && s[i + 2] == (char)quote)) {
            i++;
            continue;
          }
          unsigned k = 0;
          if (raw) {
            while (k < hashes && s[i + 1 + k] == '#')
              k++;
            if (k != hashes) {
              i++;
              continue;
            }
          }
          i += triple ? 3 : raw ? (int)hashes + 1 : 1;
          quote = 0;
          triple = raw = 0;
          hashes = 0;
          break;
        }
        i++;
      }
      span(out, capacity, &n, start, i, SYNTAX_STRING);
      continue;
    }
    if (r->line_comment &&
        !strncmp(s + i, r->line_comment, strlen(r->line_comment))) {
      i += (int)strlen(s + i);
      span(out, capacity, &n, start, i, SYNTAX_COMMENT);
      break;
    }
    if (r->block_comments && s[i] == '/' && s[i + 1] == '*') {
      i += 2;
      depth = 1;
      while (s[i] && depth) {
        if (s[i] == '*' && s[i + 1] == '/') {
          depth--;
          i += 2;
        } else if (r->nested_comments && s[i] == '/' && s[i + 1] == '*') {
          if (depth < 65535)
            depth++;
          i += 2;
        } else
          i++;
      }
      span(out, capacity, &n, start, i, SYNTAX_COMMENT);
      continue;
    }
    if (r->preprocessor && s[i] == '#') {
      int k = 0;
      while (k < i && isspace((unsigned char)s[k]))
        k++;
      if (k == i) {
        i++;
        while (isspace((unsigned char)s[i]))
          i++;
        while (word((unsigned char)s[i]))
          i++;
        span(out, capacity, &n, start, i, SYNTAX_DIRECTIVE);
        continue;
      }
    }
    int raw_start = i;
    if (r->rust_quotes && s[i] == 'b' && s[i + 1] == 'r')
      raw_start++;
    if (r->rust_quotes && s[raw_start] == 'r') {
      int k = raw_start + 1;
      while (s[k] == '#' && k - raw_start < 256)
        k++;
      if (s[k] == '"') {
        hashes = (unsigned)(k - raw_start - 1);
        raw = 1;
        quote = '"';
        i = k + 1;
        // Scan with the same multiline string rules, retaining the prefix.
        int end = i;
        while (s[end]) {
          if (s[end] == '"') {
            unsigned h = 0;
            while (h < hashes && s[end + 1 + h] == '#')
              h++;
            if (h == hashes) {
              end += (int)hashes + 1;
              quote = 0;
              raw = 0;
              hashes = 0;
              break;
            }
          }
          end++;
        }
        i = end;
        span(out, capacity, &n, start, i, SYNTAX_STRING);
        continue;
      }
    }
    if (s[i] == '"' || s[i] == '\'' || (r->backticks && s[i] == '`')) {
      if (r->rust_quotes && s[i] == '\'' && word((unsigned char)s[i + 1])) {
        int k = i + 1;
        while (word((unsigned char)s[k]))
          k++;
        if (s[k] != '\'') {
          i = k;
          continue;
        } // Rust lifetime, not a string.
      }
      quote = (unsigned char)s[i++];
      triple =
          r->triple_strings && s[i] == (char)quote && s[i + 1] == (char)quote;
      if (triple)
        i += 2;
      while (s[i]) {
        if (s[i] == '\\' && s[i + 1]) {
          i += 2;
          continue;
        }
        if ((unsigned char)s[i] == quote &&
            (!triple || (s[i + 1] == (char)quote && s[i + 2] == (char)quote))) {
          i += triple ? 3 : 1;
          quote = 0;
          triple = 0;
          break;
        }
        i++;
      }
      span(out, capacity, &n, start, i, SYNTAX_STRING);
      continue;
    }
    if (isdigit((unsigned char)s[i]) &&
        (i == 0 || !word((unsigned char)s[i - 1]))) {
      i++;
      while (word((unsigned char)s[i]) || s[i] == '.')
        i++;
      span(out, capacity, &n, start, i, SYNTAX_NUMBER);
      continue;
    }
    if (word((unsigned char)s[i]) && !isdigit((unsigned char)s[i])) {
      i++;
      while (word((unsigned char)s[i]))
        i++;
      syntax_kind kind =
          member(r->keywords, s + start, (size_t)(i - start)) ? SYNTAX_KEYWORD
          : member(r->types, s + start, (size_t)(i - start))  ? SYNTAX_TYPE
                                                              : SYNTAX_TEXT;
      int k = i;
      while (s[k] == ' ' || s[k] == '\t')
        k++;
      if (kind == SYNTAX_TEXT && s[k] == '(')
        kind = SYNTAX_FUNCTION;
      if (kind != SYNTAX_TEXT)
        span(out, capacity, &n, start, i, kind);
      continue;
    }
    i++;
  }
  if (quote && !triple && !raw && quote != '`' && !(i && s[i - 1] == '\\'))
    quote = 0;
  *state = (uint64_t)depth | ((uint64_t)quote << 16) |
           ((uint64_t)triple << 24) | ((uint64_t)raw << 25) |
           ((uint64_t)hashes << 32);
  return n;
}
