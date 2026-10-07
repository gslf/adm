#include "language.h"
static const syntax_rules rules = {
    "alignas alignof asm auto break case catch class concept const consteval "
    "constexpr constinit const_cast continue co_await co_return co_yield "
    "decltype default delete do dynamic_cast else enum explicit export extern "
    "for friend goto if inline mutable namespace new noexcept operator private "
    "protected public register reinterpret_cast requires return sizeof static "
    "static_assert static_cast struct switch template this thread_local throw "
    "try typedef typeid typename union using virtual volatile while",
    "void char char8_t char16_t char32_t wchar_t short int long float double "
    "signed unsigned bool size_t nullptr NULL true false",
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
static const char *const extensions[] = {".cpp", ".cc", ".cxx", ".c++",
                                         ".hpp", ".hh", ".hxx", ".h++",
                                         ".C",   ".H",  NULL};
static const char *const argv[] = {"clangd", "--log=error", NULL};
static const language entry = {
    "C++", "cpp",     extensions, "clangd", "ADM_LSP_CLANGD",
    argv,  highlight, NULL};
const language *language_cpp(void) { return &entry; }
