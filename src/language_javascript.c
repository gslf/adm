#include "language.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
static const syntax_rules rules = {
    "async await break case catch class const continue debugger default delete "
    "do else export extends finally for from function get if import in "
    "instanceof let new of return set static super switch this throw try "
    "typeof var void while with yield",
    "true false null undefined NaN Infinity",
    "//",
    1,
    0,
    0,
    1,
    0,
    0};
static size_t highlight(const char *line, uint64_t *state, syntax_span *out,
                        size_t capacity) {
  return language_lex(&rules, line, state, out, capacity);
}
static const char *const extensions[] = {".js", ".mjs", ".cjs", ".jsx", NULL};
static const char *const argv[] = {"typescript-language-server", "--stdio",
                                   NULL};
// npm installs a .cmd shim on Windows. Resolve its adjacent package and
// run Node directly, preserving argument boundaries without invoking cmd.exe.
static char *server_script(const char *override) {
  if (override && *override) {
    const char *ext = strrchr(override, '.');
    if (ext && (!strcmp(ext, ".mjs") || !strcmp(ext, ".js"))) {
      char *copy = malloc(strlen(override) + 1);
      if (copy)
        strcpy(copy, override);
      return copy;
    }
#ifdef _WIN32
    if (!ext || _stricmp(ext, ".cmd"))
      return NULL;
#else
    return NULL;
#endif
  }
#ifdef _WIN32
  const char *program =
      override && *override ? override : "typescript-language-server";
  DWORD length = SearchPathA(NULL, program, ".cmd", 0, NULL, NULL);
  if (!length)
    return NULL;
  const char *package =
      "node_modules\\typescript-language-server\\lib\\cli.mjs";
  char *path = malloc((size_t)length + strlen(package) + 1);
  if (!path)
    return NULL;
  DWORD resolved = SearchPathA(NULL, program, ".cmd", length + 1, path, NULL);
  if (!resolved || resolved > length) {
    free(path);
    return NULL;
  }
  char *separator = strrchr(path, '\\');
  if (!separator)
    separator = strrchr(path, '/');
  if (!separator) {
    free(path);
    return NULL;
  }
  strcpy(separator + 1, package);
  DWORD attributes = GetFileAttributesA(path);
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    free(path);
    return NULL;
  }
  return path;
#else
  return NULL;
#endif
}
static const language entry = {"JavaScript", "javascript",         extensions,
                               "javascript", "ADM_LSP_JAVASCRIPT", argv,
                               highlight,    server_script};
const language *language_javascript(void) { return &entry; }
