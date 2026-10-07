#include "language.h"
static const syntax_rules rules = {
    "and as assert async await break case class continue def del elif else "
    "except finally for from global if import in is lambda match nonlocal not "
    "or pass raise return try while with yield",
    "True False None int float complex bool str bytes list tuple dict set "
    "object type",
    "#",
    0,
    0,
    1,
    0,
    0,
    0};
static size_t highlight(const char *line, uint64_t *state, syntax_span *out,
                        size_t capacity) {
  return language_lex(&rules, line, state, out, capacity);
}
static const char *const extensions[] = {".py", ".pyw", ".pyi", NULL};
static const char *const argv[] = {"pylsp", NULL};
static const language entry = {"Python",         "python", extensions, "python",
                               "ADM_LSP_PYTHON", argv,     highlight,  NULL};
const language *language_python(void) { return &entry; }
