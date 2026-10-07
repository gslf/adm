# ][adm — User guide

## In-editor help

The following quick reference is also displayed by `C-x ?`. Use Up/Down or
PgUp/PgDown to scroll and Esc to close it.

```text
][ adm - Navigation help

C-x m      Enter MOVE mode; ESC returns to editing
h j k l    Move left / down / up / right between splits
n / p      Next / previous tab (wrap at the ends)
b          Focus the sidebar; l returns to the editor
At a split edge, movement wraps to the opposite side.

C-h C-j C-k C-l C-b C-n C-p
           Same moves immediately, without entering MOVE
Arrows move the text cursor or sidebar selection normally.
Backspace deletes; Enter inserts a newline (C-j moves down).
Text-entry prompts keep their own editing keys.

C-x f      Open File Explorer with focus
C-x g      Open Git Manager with focus
C-x l      Go to Line
C-s / C-x s  Search current file; C-x S searches workspace
C-x r / R  Search and replace in file / workspace
Tab        Toggle TEXT / REGEX while entering a query
Up / Down  Previous / next occurrence (C-r / C-s)
Enter      Accept query; then enter replacement if needed
y / n / a  Replace one / next / all after replacement entry
Workspace replace-all confirms and writes saved files.
C-t        New empty tab (also C-x t)
C-q        Empty split above-below (also C-x q)
C-u        Empty split side by side (also C-x u)
C-x N      Create a named file in an empty split
C-x } / {  Grow / shrink focused split or sidebar width
C-x c      Close current split
C-x k      Close current tab and all its splits
Unsaved last references require confirmation before closing.
C-z / M-z  Undo / redo (also C-x z / C-x Z)
C-x C-s    Save; C-x C-c quits
Quick tab/split creation also works in MOVE.

M-x        LSP menu (Alt-x or Esc then x)
s/x/r      Start / stop / restart language server
h/d/c      Symbol information / definition / completion
n/p        Next / previous diagnostic
N/f        Rename symbol / format current file
a/t/?      Automatic startup / syntax colours / LSP help

Up/Down or PgUp/PgDown scroll; ESC closes this help
```

## Controls

`C-` means Ctrl; `M-` means Alt, or Esc followed by the key.

| Key | Action |
|-----|--------|
| Arrows / C-f | Move text cursor; C-f moves right |
| M-b / M-f | Previous / next word |
| C-a / C-e | Start / end of line |
| M-v / C-v | Page up / down |
| M-< / M-> | Start / end of file |
| C-SPC | Toggle selection mode |
| M-w / C-w / C-y | Copy / cut / paste |
| Backspace / C-d | Delete backward / forward |
| C-z / M-z | Undo / redo (also C-x z / C-x Z) |
| C-s / C-x s | Search text in the current buffer; Tab toggles regex |
| C-x S | Search text or regex in the workspace |
| C-x r / C-x R | Search and replace in the current file / workspace |
| C-g / Esc | Cancel selection or prompt; return from sidebar to editor |
| C-x C-s / C-x C-c | Save current buffer / quit |
| C-x l | Go to Line |
| C-q / C-x q | New empty split above-below |
| C-u / C-x u | New empty split side by side |
| C-x m | Enter MOVE mode until Esc |
| C-h / C-j / C-k / C-l | Move focus left / down / up / right |
| C-b | Focus sidebar |
| C-n / C-p | Next / previous tab |
| C-x ? | Navigation help |
| M-x | Separate LSP menu (Alt-x or Esc then x) |
| C-x ] / C-x [ | Grow / shrink split height |
| C-x } / C-x { | Grow / shrink sidebar width when focused; otherwise split width |
| C-x c | Close current split |
| C-t / C-x t | New empty tab with the File Explorer focused |
| C-x N | Create a named file in an empty split |
| C-x k | Close current tab and all its splits; the last tab quits |
| C-x f | Open File Explorer with focus |
| C-x g | Open Git Manager with focus |

`C-x m` enters MOVE mode, shown in the bottom bar. Use `h/j/k/l` to focus the
split to the left/below/above/right, `n/p` to switch tabs, and `b` to focus the
sidebar. From the sidebar, `l` returns to the current editor split. Esc leaves
MOVE mode; other keys do not edit text while MOVE is active.
`C-t`, `C-q` and `C-u` can create tabs and splits without leaving MOVE. Split movement wraps
to the opposite edge, preferring a split in the same row or column; tab navigation
also wraps. With a single split in a row or column, moving along that axis keeps
the current split selected. It follows split geometry even when a
small terminal displays only the active split.

The Ctrl versions (`C-h/j/k/l/b/n/p`) perform the same actions without entering
MOVE. They replace the previous Emacs cursor/backspace shortcuts; arrows and
Backspace retain those editing functions. Enter inserts a newline; `C-j` moves
between splits. Search, Go to Line, filename and Git text-entry prompts retain
ownership of their input keys. `C-x ?` opens scrollable navigation help.

Quick creation uses ordinary Ctrl keys: `C-q` for an above-below split,
`C-u` for a side-by-side split, and `C-t` for a tab. The Command Center uses the
same letters: `C-x q`, `C-x u`, and `C-x t`. No extended keyboard protocol is
required. `C-w` remains cut, `C-SPC` remains selection, and `C-r` remains previous
search match. The old split suffixes `2/3` and uppercase tab suffix `T` are removed.

The MOVE bar uses spaced labels for splits, tabs and sidebar. It switches to
shorter labels on narrow terminals; `b` focuses the sidebar, `l` returns to the
editor, and Esc exits MOVE. The immediate Ctrl shortcut for sidebar focus is
`C-b`. `s` has no MOVE binding; `C-s` stays incremental search in editing mode.

## Tabs and buffers

Starting adm without a filename shows the empty welcome split and focuses the
File Explorer. A missing filename supplied as an argument starts an empty
document that is created on save.

Each tab holds up to four splits with its own split layout, focused split, cursors,
selections, and scroll positions. New tabs and new splits start empty, with
no document or buffer attached. They display the centered adm logo and focus the
File Explorer so Enter can open a file into the selected split. Existing splits
keep their documents. The top bar shows tab numbers, highlights the active tab, and marks
tabs containing modified buffers with `*`; it keeps the active tab visible when
there are more tabs than fit on screen.

Opening the same file in another tab or split shares its text and saved state.
Edits immediately update every view, including cursor and selection positions in
inactive tabs. Relative and absolute paths share the same document. On POSIX, symlink aliases
and hard links also share while they reference the same disk file.
Saving uses the document's canonical path. Atomic replacement can separate hard
links on disk.

There is no background buffer list: a document lives only while a tab's split
references it. Closing a tab releases every split in it. Closing splits, closing
tabs, and replacing a view ask before discarding a modified document's last
references. Cancelling preserves the entire layout. Quit checks every tab, and Git worktree operations check unsaved
files across all tabs. Delayed Git diffs target the original tab and split without
switching away from another tab; results for closed or replaced views are rejected.

Files load and save in a stream, so neither operation builds a second whole-file
copy. Shared views also reuse character-count and line-lookup caches. A failed
save preserves the original file and leaves the shared buffer modified. Text is
still held in memory; available RAM limits file size, and exceptionally long lines
and complex regex searches can still require more time. The existing text format converts CRLF to LF
and saves each buffer line with a trailing newline.

## Undo and redo

`C-z` undoes an edit; `M-z` redoes it. The Command Center provides `C-x z`
and `C-x Z` when the corresponding action is available. `M-z` also works as
Esc followed by `z`, without an extended terminal keyboard protocol.

History belongs to the document and is shared across all its tabs and splits.
Typing consecutive word characters forms one action; movement, punctuation,
whitespace, a pause, or saving starts another. Consecutive deletions are grouped
in bounded runs. Paste, cut, selection replacement and file replace-all are
single actions. Undo restores the invoking split’s cursor, selection and scroll
position, and rebases the other views. A new edit after undo discards redo.
Returning to the saved state clears the modified marker, even after undo or redo.

The editor keeps up to 1,000 actions per document. It records changed text in a
temporary disk journal rather than copying entire files; abandoned history is
periodically reclaimed. History lasts until the
last view closes or the document reloads from disk. The journal is removed when
the document closes. If an edit cannot be recorded, it is cancelled; if replay
fails, text and history remain unchanged. Workspace replacement writes saved
files directly and is not covered by buffer undo.

## Creating files

In an empty split, press `C-x N`, enter a name, and press Enter to create and open
an empty file. Esc or `C-g` cancels and returns focus to the File Explorer. Simple
names use the selected Explorer folder, or the selected file's parent folder;
the initial selection is the directory where adm was launched. Names containing
directory separators must be absolute paths and ignore the Explorer selection.
The parent directory must already exist. Existing files are never overwritten;
errors keep the name prompt open so it can be corrected.


## Sidebar

The sidebar shows either the File Explorer or Git Manager. Opening either sidebar
always gives it keyboard focus. `C-b` focuses the current sidebar (opening the last
used sidebar, or Files, if necessary); `C-l` returns to the selected editor split.
Opening an already visible sidebar focuses it again.

While the sidebar has focus, `C-x }` expands it and `C-x {` shrinks it, in both
File Explorer and Git Manager. Focus stays in the sidebar. With editor focus,
the same commands resize the selected split. The chosen sidebar width is shared
between both modes and retained when reopening it or resizing the terminal;
its displayed width is clamped to leave space for the editor.

In the **File Explorer** the tree starts in the directory where adm was launched. Use arrows to navigate, expand and collapse folders. Enter toggles folders, opening a file with Enter targets the selected split and returns focus to the editor.

In the **Git Manager**, Enter opens the selected staged or unstaged diff in the active split, in read-only mode, **o** opens the file for editing. Diffs show removed lines on dark red, added lines on dark green and unchanged context on grey. Git uses saved files, so commits include only staged changes.


## Search and replace

The workspace is fixed to the directory where adm started, even when you open
files outside it or navigate into Explorer subdirectories. `C-s` or `C-x s`
searches the current in-memory buffer, including unsaved edits. `C-x S` searches
saved text files recursively in that workspace. Searches are case sensitive.
Literal text is the default: dots, brackets and other regex characters are
ordinary text. Press Tab while entering the query to switch TEXT / REGEX.

Enter the query in the bottom bar. It shows the current occurrence and total;
`...` means the count is still growing. Up/Down or `C-r`/`C-s` navigate matches,
wrapping at the ends. Enter accepts a file search; Esc restores its original
cursor. In workspace search Enter opens the selected result and keeps navigation
active; Esc ends search and restores the normal Explorer. Search input supports
Left/Right, Home/End, Delete and Backspace, including UTF-8 text.

Workspace search replaces the Explorer tree with matching files only. Each file
uses two rows: its name and occurrence count, followed by its absolute path.
In the results phase `C-b` focuses that list: Up/Down select files, Left/Right
scroll their full paths horizontally and Enter opens the first occurrence in the
selected file. `C-l` returns to the editor; Up/Down then walk individual matches.
Opening a result uses the usual dirty-buffer confirmation and shares documents
already open in other tabs or splits. Results are checked against the current
buffer before highlighting or replacement, so a stale position cannot replace
unrelated text. Workspace counts describe saved files; unsaved edits are searched
with the current-file command. File search waits until an active Git operation
finishes so asynchronous reloads cannot invalidate the buffer during scanning.

`C-x r` (file) and `C-x R` (workspace) first collect the query. Enter moves to the
replacement field, where an empty replacement means deletion. Enter again shows
`y` to replace the selected occurrence, `n` for next, and `a` for all. Replacement
text is literal: regex captures, backreferences and escape substitutions are not
expanded. Replacing one workspace result requires opening it with Enter first;
that edits the shared buffer and leaves it unsaved, just like typing.

File replace-all changes the shared buffer and leaves it unsaved. It builds each
changed line once and preserves other splits' cursor and selection positions.
Esc stops remaining replacements; changes already applied remain in the buffer.
Workspace replace-all requires a completed search, asks for confirmation, and
refuses to run while workspace buffers are dirty or Git is active. It searches again in the matched files
and writes each changed file through an exclusive temporary sibling, checking
file identity, size and timestamps against the search snapshot and again before
committing. Files changed since the search are skipped. It preserves BOM, CRLF and
final-newline style. Each file is committed individually; cancellation or a
failure can leave some files already replaced. The final bar reports changed and
skipped files. Clean open documents reload afterward, keeping shared views.

Regex supports UTF-8 literals, `.`, `^`, `$`, character classes and ranges,
negated classes, `\d`, `\w`, `\s` (and their uppercase inverses), `*`, `+`, `?`,
parenthesized groups and `|` alternatives. Matches are line-local, non-overlapping,
leftmost and longest; zero-length matches advance one code point so they cannot
loop forever. Counted repetitions, lookaround and backreferences are not supported.
Malformed or unsupported syntax reports an invalid regex. Query and replacement
fields each accept up to 255 UTF-8 bytes. Matching uses a bounded
state machine rather than recursive backtracking.

Workspace search uses a bounded native worker pool on POSIX and Windows, without
requiring ripgrep or an extended terminal protocol. It chooses up to eight scanners
from the available logical CPUs and queues at most 256 file paths. Files appear in
completion order; each file's occurrences remain contiguous and ordered by position.
Results from different workers never interleave within a file. A worker validates
its file before publishing results, so late binary content or a changed file cannot
invalidate another worker's matches. Partial results appear as files complete and
the TUI stays available. A single huge file can take longer to produce its first
result. Changing the query cancels the previous scan and its queued work.

Literal search uses runtime-selected AVX2/SSE2 on x86, NEON on AArch64, and a portable
`memchr` fallback. Dense false candidates switch to a linear-time matcher so
repetitive text cannot cause unbounded verification work. Mandatory ASCII prefixes
and character-class byte tables filter regex candidates; plain literal regexes use the literal matcher directly. Common lines are scanned in the read
chunk without a separate copy; lines spanning chunks use a growable scratch line.
Each scanner retains bounded hit batches and spills dense results to a temporary
disk index. RAM use does not grow with the number of occurrences, but exceptionally
long lines need scratch memory. Current-buffer searches and workspace writes use
one worker; shared buffer lifetimes and replacement checks retain their usual rules.

Set `ADM_SEARCH_THREADS=1` to use a serial workspace scan, useful for slow disks or
very small workspaces. Values from 1 through 32 override the automatic worker count;
invalid values use the default. More threads do not always improve performance:
filesystem latency, tiny files, dense matches and regex complexity can dominate.
Huge workspaces still take time to read.

For reproducible warm-cache measurements, run
`python3 tests/bench_search.py --mib 128 --repeat 5` (or `make bench-search`). It verifies
occurrence totals and measures traversal plus index creation with 1/2/4/8/automatic
workers. When installed, GNU grep and ripgrep provide additional reference timings;
their output work differs from adm's navigable index. See [search benchmark results](tests/SEARCH_PERFORMANCE.md)
for the measured corpus, limits and implementation references.
Hidden and ignored text files are included. `.git` metadata, symbolic links,
Windows reparse points and special devices are excluded. Workspace replacement
skips hard-linked files to preserve aliases and shared-buffer consistency. NUL-containing binary
files, unreadable or changing files are skipped and reported; temporary-index
failures produce an explicit error instead of claiming a complete count.


## Languages, syntax colours and LSP

adm detects C, C++, JavaScript, Rust and Python from the filename extension.
Syntax colours are enabled by default and work without a language server.
Headers `.h` use C; `.hpp`, `.hh` and `.hxx` use C++. JavaScript includes `.js`,
`.mjs`, `.cjs` and `.jsx`; Python includes `.py`, `.pyw` and `.pyi`.
The highlighter is a lightweight lexical scanner for keywords, types, strings,
comments, numbers, directives and function names, rather than a semantic parser.

`M-x` opens the separate **LSP** menu. It uses ordinary Alt-x, or Esc followed
immediately by x, over SSH; no extended keyboard protocol is required.
The commands refer to the document in the current split, including when the
sidebar has focus. LSP commands are not in the `C-x` Command Center.

| Key after M-x | Action |
|---------------|--------|
| s | Start the current language's server |
| x | Stop that server |
| r | Restart that server |
| h | Show information about the symbol at the cursor |
| d | Show definitions; select with Up/Down and open with Enter |
| c | Request completion; select with Up/Down and insert with Enter |
| n / p | Next / previous diagnostic, wrapping at the ends |
| N | Rename the symbol at the cursor; enter its new name in the bottom bar |
| f | Format the current file, using four-space indentation options |
| a | Toggle automatic startup for languages in visible tabs and splits |
| t | Toggle syntax colours |
| ? | Show LSP help |
| Esc / C-g | Close the menu or result, cancel a name prompt or pending request |

Hover information and help scroll with Up/Down. Menus scroll to fit small
terminals. Diagnostics display the server's message and severity in the bottom
bar. Servers can decline individual features; adm reports that without changing
text. Names and other language-specific rules are validated by the server.

Servers are started on demand with `M-x s`. Automatic startup is initially off;
`M-x a` enables it for this editor session. Stopping a server suppresses automatic
restart until it is explicitly started, restarted, or automatic startup is
re-enabled. C and C++ share one clangd session. The workspace root is always the
folder from which adm was launched, regardless of the File Explorer's location.

Install the appropriate server separately and make its executable available on
`PATH`, or set the corresponding environment variable to its executable path:

| Language | Default command | Executable override | Server documentation |
|----------|-----------------|---------------------|----------------------|
| C / C++ | `clangd --log=error` | `ADM_LSP_CLANGD` | [clangd installation and project setup](https://clangd.llvm.org/installation) |
| JavaScript | `typescript-language-server --stdio` | `ADM_LSP_JAVASCRIPT` | [TypeScript Language Server](https://github.com/typescript-language-server/typescript-language-server) |
| Rust | `rust-analyzer` | `ADM_LSP_RUST` | [rust-analyzer installation](https://rust-analyzer.github.io/book/installation.html) |
| Python | `pylsp` | `ADM_LSP_PYTHON` | [Python LSP Server](https://github.com/python-lsp/python-lsp-server) |

Overrides contain an executable path, not a shell command; adm keeps the listed
arguments. For JavaScript, the server also needs Node.js and TypeScript installed.
On Windows adm resolves the npm `.cmd` launcher to its adjacent package and
runs Node directly. `ADM_LSP_JAVASCRIPT` may also point to the package's
`lib/cli.mjs` (or a `.js` entry point) on either platform. C/C++
projects should provide `compile_commands.json` as described in clangd's guide.
A missing or crashed server leaves editing and syntax colours available.

A document shared by several tabs or splits is synchronized once, with
incremental changes where supported. UTF-8 and UTF-16 position encodings are
negotiated. Changes from typing, Undo, Redo, completion, rename and formatting
share the same buffer. Closing its last split sends `didClose` and releases
its LSP and highlighting data along with the document.

Completion, rename and formatting create undoable, unsaved buffer edits.
A multi-file rename prepares and validates **every** affected file before
changing any text. Previously unopened files appear in new visible tabs;
there are no persistent hidden buffers or automatic disk writes. Save each
changed document normally. Undo/Redo operates per document, so a rename across
several files is undone separately in each one. Responses for changed documents,
moved cursors or different splits are ignored. Open documents participating in
rename must still match the versions captured when the request was sent.

To keep memory and input latency bounded, LSP synchronization pauses for
files larger than **1 MiB** or containing invalid UTF-8. They remain editable;
syntax highlighting continues within its own budget. Colouring scans at most
256 KiB and approximately 2 ms per preparation call, caches multiline state in
the shared document and updates it after edits. Lines over 64 KiB display as
plain text; a jump into an uncached part of a large file can initially use local
colouring until the multiline state catches up.

This first LSP implementation supports text edits rather than file creation,
renaming or deletion operations. Workspace edits are limited to 128 files,
8,192 edits, 8 MiB per affected file and 32 MiB of original text overall; JSON-RPC
messages and queued input are capped at 8 MiB. Overlapping edits, invalid
positions, stale versions or changed disk files are rejected as a whole.
Completion snippets and additional edits are declined, rather than partially
applied. Initialization and interactive requests time out, and server I/O uses
bounded, nonblocking queues so a server cannot block typing.

See [LANGUAGES.md](LANGUAGES.md) for adding a language adapter to the shared registry.
