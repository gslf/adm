#include "language.h"
static const syntax_rules rules = {
    "as async await break const continue crate dyn else enum extern fn for if "
    "impl in let loop match mod move mut pub ref return self Self static "
    "struct super trait type union unsafe use where while",
    "bool char str i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f32 f64 "
    "true false Option Result Some None Ok Err",
    "//",
    1,
    1,
    0,
    0,
    0,
    1};
static size_t highlight(const char *line, uint64_t *state, syntax_span *out,
                        size_t capacity) {
  return language_lex(&rules, line, state, out, capacity);
}
static const char *const extensions[] = {".rs", NULL};
static const char *const argv[] = {"rust-analyzer", NULL};
static const language entry = {"Rust",         "rust", extensions, "rust",
                               "ADM_LSP_RUST", argv,   highlight,  NULL};
const language *language_rust(void) { return &entry; }
