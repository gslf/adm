# Language adapters

Each supported language lives in `src/language_<name>.c`. The adapter returns a
static `language` descriptor with extensions, LSP language ID, executable and
arguments, server family, executable override variable, and a highlight callback.
All descriptors and arrays must outlive the editor. The registry holds up to 32
adapters; the first matching extension wins.

To add another language:

1. Add its source file with a descriptor and highlight callback.
2. Declare its descriptor accessor in `src/language.h`.
3. Register it in `languages_init()` in `src/language.c`.

The Makefile automatically includes new C sources. No changes to the transport,
editor commands, buffer management or renderer are needed. A minimal adapter:

```c
#include "language.h"

static const syntax_rules rules = {
    .keywords = "if else return",
    .types = "int bool",
    .line_comment = "//",
    .block_comments = 1,
};
static size_t highlight(const char *line, uint64_t *state,
                        syntax_span *out, size_t capacity) {
  return language_lex(&rules, line, state, out, capacity);
}
static const char *const extensions[] = {".example", NULL};
static const char *const argv[] = {"example-language-server", "--stdio", NULL};
static const language entry = {
    "Example", "example", extensions, "example", "ADM_LSP_EXAMPLE",
    argv, highlight, NULL
};
const language *language_example(void) { return &entry; }
```

A custom highlight callback may replace the shared lexer. Its `uint64_t` state
contains all multiline context; update it even when `out == NULL` and capacity
is zero. Emit ordered, nonoverlapping byte spans, bounded by capacity. A source
line is UTF-8 but may contain invalid bytes; the scanner must retain those bytes.
Callbacks must not keep per-view state or scan the whole document themselves.

`server_key` identifies a process family: C and C++ use `clangd` to share a single
server while sending distinct document language IDs. `server_env` overrides only
the executable; `server_argv` supplies fixed arguments, without a shell.

Common implementation:

- `language.c`: registration, extension matching and configurable lexical scanner.
- `syntax.c`: document-owned, incremental multiline state cache and scan budgets.
- `json.c`: bounded JSON parser and message builder.
- `process.c`: POSIX/Windows child lifecycle and nonblocking duplex transport.
- `lsp.c`: protocol framing, initialization, capabilities, encoding, shared
  document synchronization, stale-response guards and the M-x interface.
- `lsp_edits.c`: preflight and atomic multi-file text edits, visible tabs and Undo.

Use `tests/languages.c` for lexical fixtures, `tests/lsp.c` for a fragmented,
versioned fake server, and `tests/lsp_edits.c` for atomicity and fault injection.
Run the full regression suite with `make test`. Adding an adapter does not install
its server: list the executable and project prerequisites in `HELP.md`.

An optional `server_script` callback can return an owned Node script path.
The common launcher runs it with `node` and the adapter's fixed flags, then frees
that path. JavaScript uses this to resolve npm's Windows `.cmd` shim to the
package's `lib/cli.mjs`; no shell interprets server arguments. Other adapters
leave this callback NULL.
