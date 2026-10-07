#include "language.h"
static const syntax_rules rules = {
    "auto break case const continue default do else enum extern for goto if "
    "inline register restrict return sizeof static struct switch typedef union "
    "volatile while _Alignas _Alignof _Atomic _Generic _Noreturn "
    "_Static_assert _Thread_local",
    "void char short int long float double signed unsigned bool size_t NULL "
    "true false",
    "//",
    1,
    0,
    0,
    0,
    1,
    0};
static size_t highlight(const char *line, uint64_t *state, syntax_span *out,
                        size_t capacity) {
  return language_lex(&rules, line, state, out, capacity);
}
static const char *const extensions[] = {".c", ".h", NULL};
static const char *const argv[] = {"clangd", "--log=error", NULL};
static const language entry = {
    "C", "c", extensions, "clangd", "ADM_LSP_CLANGD", argv, highlight, NULL};
const language *language_c(void) { return &entry; }
