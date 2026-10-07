#include "json.h"
#include "utf8.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define JSON_LIMIT (8u * 1024u * 1024u)
typedef struct parser {
  json_value *v;
  size_t at, bytes, capacity;
} parser;
void json_bytes(json_buf *b, const char *s, size_t bytes) {
  if (b->failed)
    return;
  if (bytes > JSON_LIMIT - b->length) {
    b->failed = 1;
    return;
  }
  size_t need = b->length + bytes + 1;
  if (need > b->capacity) {
    size_t cap = b->capacity ? b->capacity * 2 : 256;
    if (cap < need)
      cap = need;
    char *p = realloc(b->data, cap);
    if (!p) {
      b->failed = 1;
      return;
    }
    b->data = p;
    b->capacity = cap;
  }
  memcpy(b->data + b->length, s, bytes);
  b->length += bytes;
  b->data[b->length] = 0;
}
void json_add(json_buf *b, const char *s) { json_bytes(b, s, strlen(s)); }
void json_number(json_buf *b, int64_t n) {
  char text[32];
  snprintf(text, sizeof text, "%lld", (long long)n);
  json_add(b, text);
}
void json_quote_bytes(json_buf *b, const char *s, size_t n) {
  json_add(b, "\"");
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '"' || c == '\\') {
      char escape[2] = {'\\', (char)c};
      json_bytes(b, escape, 2);
    } else if (c < 32) {
      char escape[7];
      snprintf(escape, sizeof escape, "\\u%04x", c);
      json_add(b, escape);
    } else
      json_bytes(b, s + i, 1);
  }
  json_add(b, "\"");
}
void json_quote(json_buf *b, const char *s) {
  json_quote_bytes(b, s, strlen(s));
}
void json_buf_free(json_buf *b) {
  free(b->data);
  *b = (json_buf){0};
}
static int hex(char c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                : -1;
}
static void white(parser *p) {
  while (p->at < p->bytes && strchr(" \r\n\t", p->v->text[p->at]))
    p->at++;
}
static size_t token(parser *p, json_type type, size_t start) {
  if (p->v->count >= 262144)
    return JSON_NONE;
  if (p->v->count == p->capacity) {
    size_t cap = p->capacity ? p->capacity * 2 : 128;
    json_token *t = realloc(p->v->tokens, cap * sizeof *t);
    if (!t)
      return JSON_NONE;
    p->v->tokens = t;
    p->capacity = cap;
  }
  size_t i = p->v->count++;
  p->v->tokens[i] = (json_token){type, start, 0, 0};
  return i;
}
static size_t parse_value(parser *p, int depth) {
  if (depth > 64)
    return JSON_NONE;
  white(p);
  if (p->at >= p->bytes)
    return JSON_NONE;
  const char *s = p->v->text;
  char c = s[p->at];
  json_type type = c == '{'               ? JSON_OBJECT
                   : c == '['             ? JSON_ARRAY
                   : c == '"'             ? JSON_STRING
                   : c == 't' || c == 'f' ? JSON_BOOL
                   : c == 'n'             ? JSON_NULL
                                          : JSON_NUMBER;
  size_t i = token(p, type, p->at);
  if (i == JSON_NONE)
    return i;
  if (type == JSON_OBJECT || type == JSON_ARRAY) {
    p->at++;
    white(p);
    char close = type == JSON_OBJECT ? '}' : ']';
    if (p->at < p->bytes && s[p->at] == close)
      p->at++;
    else
      for (;;) {
        if (type == JSON_OBJECT) {
          white(p);
          if (p->at >= p->bytes || s[p->at] != '"' ||
              parse_value(p, depth + 1) == JSON_NONE)
            return JSON_NONE;
          white(p);
          if (p->at >= p->bytes || s[p->at++] != ':')
            return JSON_NONE;
        }
        if (parse_value(p, depth + 1) == JSON_NONE)
          return JSON_NONE;
        white(p);
        if (p->at >= p->bytes)
          return JSON_NONE;
        char next = s[p->at++];
        if (next == close)
          break;
        if (next != ',')
          return JSON_NONE;
      }
  } else if (type == JSON_STRING) {
    p->at++;
    int closed = 0;
    while (p->at < p->bytes) {
      c = s[p->at++];
      if (c == '"') {
        closed = 1;
        break;
      }
      if ((unsigned char)c < 32)
        return JSON_NONE;
      if ((unsigned char)c >= 128) {
        // Decode from a bounded, terminated copy: protocol input need not
        // have a terminator and a truncated sequence must not read past it.
        char sequence[5] = {0};
        size_t available = p->bytes - (p->at - 1);
        if (available > 4)
          available = 4;
        memcpy(sequence, s + p->at - 1, available);
        int cp, bytes = utf8_decode(sequence, &cp);
        if (bytes == 1)
          return JSON_NONE;
        p->at += (size_t)bytes - 1;
      }
      if (c == '\\') {
        if (p->at >= p->bytes)
          return JSON_NONE;
        char escape = s[p->at++];
        if (escape == 'u') {
          for (int n = 0; n < 4; n++)
            if (p->at >= p->bytes || hex(s[p->at++]) < 0)
              return JSON_NONE;
        } else if (!strchr("\"\\/bfnrt", escape))
          return JSON_NONE;
      }
    }
    if (!closed)
      return JSON_NONE;
  } else if (type == JSON_NULL || type == JSON_BOOL) {
    const char *literal = c == 'n' ? "null" : c == 't' ? "true" : "false";
    size_t n = strlen(literal);
    if (n > p->bytes - p->at || memcmp(s + p->at, literal, n))
      return JSON_NONE;
    p->at += n;
  } else {
    if (c == '-')
      p->at++;
    if (p->at >= p->bytes || !isdigit((unsigned char)s[p->at]))
      return JSON_NONE;
    if (s[p->at] == '0')
      p->at++;
    else
      while (p->at < p->bytes && isdigit((unsigned char)s[p->at]))
        p->at++;
    if (p->at < p->bytes && s[p->at] == '.') {
      p->at++;
      size_t begin = p->at;
      while (p->at < p->bytes && isdigit((unsigned char)s[p->at]))
        p->at++;
      if (p->at == begin)
        return JSON_NONE;
    }
    if (p->at < p->bytes && (s[p->at] == 'e' || s[p->at] == 'E')) {
      p->at++;
      if (p->at < p->bytes && (s[p->at] == '+' || s[p->at] == '-'))
        p->at++;
      size_t begin = p->at;
      while (p->at < p->bytes && isdigit((unsigned char)s[p->at]))
        p->at++;
      if (p->at == begin)
        return JSON_NONE;
    }
  }
  p->v->tokens[i].end = p->at;
  p->v->tokens[i].next = p->v->count;
  return i;
}
int json_parse(json_value *v, const char *s, size_t n) {
  *v = (json_value){.text = s};
  if (n > JSON_LIMIT || memchr(s, 0, n))
    return 0;
  parser p = {.v = v, .bytes = n};
  size_t root = parse_value(&p, 0);
  white(&p);
  if (root == JSON_NONE || p.at != n) {
    json_free(v);
    return 0;
  }
  return 1;
}
void json_free(json_value *v) {
  free(v->tokens);
  *v = (json_value){0};
}
int json_is(const json_value *v, size_t i, json_type type) {
  return i < v->count && v->tokens[i].type == type;
}
size_t json_first(const json_value *v, size_t i) {
  return json_is(v, i, JSON_ARRAY) && i + 1 < v->tokens[i].next ? i + 1
                                                                : JSON_NONE;
}
char *json_string(const json_value *v, size_t i) {
  if (!json_is(v, i, JSON_STRING))
    return NULL;
  const json_token *t = &v->tokens[i];
  json_buf b = {0};
  for (size_t n = t->start + 1; n < t->end - 1; n++) {
    char c = v->text[n];
    if (c != '\\') {
      json_bytes(&b, &c, 1);
      continue;
    }
    c = v->text[++n];
    if (c == 'u') {
      int cp = 0;
      for (int k = 0; k < 4; k++)
        cp = (cp << 4) | hex(v->text[++n]);
      if (cp >= 0xd800 && cp <= 0xdbff) {
        if (n + 6 >= t->end || v->text[n + 1] != '\\' || v->text[n + 2] != 'u')
          goto fail;
        int low = 0;
        n += 2;
        for (int k = 0; k < 4; k++)
          low = (low << 4) | hex(v->text[++n]);
        if (low < 0xdc00 || low > 0xdfff)
          goto fail;
        cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
      }
      if (!cp || (cp >= 0xdc00 && cp <= 0xdfff))
        goto fail;
      char seq[4];
      int bytes = utf8_encode(cp, seq);
      if (!bytes)
        goto fail;
      json_bytes(&b, seq, (size_t)bytes);
    } else {
      c = c == 'b'   ? '\b'
          : c == 'f' ? '\f'
          : c == 'n' ? '\n'
          : c == 'r' ? '\r'
          : c == 't' ? '\t'
                     : c;
      json_bytes(&b, &c, 1);
    }
  }
  if (b.failed)
    goto fail;
  if (!b.data) {
    b.data = malloc(1);
    if (b.data)
      b.data[0] = 0;
  }
  return b.data;
fail:
  json_buf_free(&b);
  return NULL;
}
size_t json_get(const json_value *v, size_t i, const char *key) {
  if (!json_is(v, i, JSON_OBJECT))
    return JSON_NONE;
  size_t end = v->tokens[i].next;
  for (size_t n = i + 1; n < end;) {
    char *name = json_string(v, n);
    if (!name)
      return JSON_NONE;
    size_t value = n + 1;
    int equal = !strcmp(name, key);
    free(name);
    if (equal)
      return value;
    n = v->tokens[value].next;
  }
  return JSON_NONE;
}
int json_int(const json_value *v, size_t i, int64_t *n) {
  if (!json_is(v, i, JSON_NUMBER))
    return 0;
  size_t bytes = v->tokens[i].end - v->tokens[i].start;
  if (bytes >= 32)
    return 0;
  char text[32];
  memcpy(text, v->text + v->tokens[i].start, bytes);
  text[bytes] = 0;
  char *end;
  errno = 0;
  long long value = strtoll(text, &end, 10);
  if (errno || *end)
    return 0;
  *n = (int64_t)value;
  return 1;
}
int json_true(const json_value *v, size_t i) {
  return json_is(v, i, JSON_BOOL) && v->text[v->tokens[i].start] == 't';
}
