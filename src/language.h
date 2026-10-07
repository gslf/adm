#ifndef LANGUAGE_H
#define LANGUAGE_H
#include <stddef.h>
#include <stdint.h>

typedef enum syntax_kind {
  SYNTAX_TEXT,
  SYNTAX_KEYWORD,
  SYNTAX_TYPE,
  SYNTAX_STRING,
  SYNTAX_COMMENT,
  SYNTAX_NUMBER,
  SYNTAX_DIRECTIVE,
  SYNTAX_FUNCTION
} syntax_kind;
typedef struct syntax_span {
  int start, end;
  syntax_kind kind;
} syntax_span;
typedef struct syntax_rules {
  const char *keywords, *types, *line_comment;
  int block_comments, nested_comments, triple_strings, backticks, preprocessor;
  int rust_quotes;
} syntax_rules;
typedef struct language {
  const char *name, *id;
  const char *const *extensions;
  const char *server_key, *server_env;
  const char *const *server_argv;
  size_t (*highlight)(const char *line, uint64_t *state, syntax_span *out,
                      size_t capacity);
  // Optional native launcher resolution (e.g. npm's Windows .cmd shim).
  // A non-NULL result is an owned Node script path; the common client
  // launches it with node and the remaining server_argv arguments.
  char *(*server_script)(const char *override);
} language;
int language_register(const language *entry);
void languages_init(void);
const language *language_for_filename(const char *filename);
size_t language_count(void);
const language *language_at(size_t index);
size_t language_lex(const syntax_rules *rules, const char *line,
                    uint64_t *state, syntax_span *out, size_t capacity);
const language *language_c(void);
const language *language_cpp(void);
const language *language_javascript(void);
const language *language_rust(void);
const language *language_python(void);
#endif
