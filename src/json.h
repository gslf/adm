#ifndef ADM_JSON_H
#define ADM_JSON_H
#include <stddef.h>
#include <stdint.h>
typedef enum json_type {
  JSON_OBJECT,
  JSON_ARRAY,
  JSON_STRING,
  JSON_NUMBER,
  JSON_BOOL,
  JSON_NULL
} json_type;
typedef struct json_token {
  json_type type;
  size_t start, end, next;
} json_token;
typedef struct json_value {
  const char *text;
  json_token *tokens;
  size_t count;
} json_value;
typedef struct json_buf {
  char *data;
  size_t length, capacity;
  int failed;
} json_buf;
int json_parse(json_value *v, const char *text, size_t bytes);
void json_free(json_value *v);
size_t json_get(const json_value *v, size_t object, const char *key);
size_t json_first(const json_value *v, size_t array);
int json_is(const json_value *v, size_t i, json_type type);
int json_int(const json_value *v, size_t i, int64_t *number);
int json_true(const json_value *v, size_t i);
char *json_string(const json_value *v, size_t i);
void json_add(json_buf *b, const char *s);
void json_bytes(json_buf *b, const char *s, size_t bytes);
void json_quote(json_buf *b, const char *s);
void json_quote_bytes(json_buf *b, const char *s, size_t bytes);
void json_number(json_buf *b, int64_t number);
void json_buf_free(json_buf *b);
#define JSON_NONE ((size_t)-1)
#endif
