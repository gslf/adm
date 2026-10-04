# ][adm

A minimal terminal text editor that does **one** thing, edit text, and does it well.

![adm ScreenShot](res/ss.png)


- **Cross-platform** — POSIX (Linux, macOS, *BSD) and Windows.
- **No dependencies** — only the C standard library and the OS terminal API.
- **Small** — a handful of readable C files.

## Build

```
make
```

Produces the `adm` executable (needs a C11 compiler). Clean with `make clean`.
Run the control and layout regression tests with `make test` (requires Python 3).
On POSIX this also tests the real editor in a pseudo-terminal, including rendering
and terminal resize events.

## Usage

```
adm [file]
```

Opens `file` for editing. If it does not exist yet, the buffer starts empty and
the file is created on save. With no argument you get an empty buffer.


`C-` means Ctrl; `M-` means Alt (or Esc followed by the key).

| Key                             | Action                              |
|---------------------------------|-------------------------------------|
| C-p / C-n / C-b / C-f            | Move up / down / left / right        |
| M-b / M-f                       | Previous / next word                |
| C-a / C-e                       | Start / end of the line              |
| M-v / C-v                       | One screen up / down                 |
| M-< / M->                       | Start / end of the file              |
| C-SPC                           | Selection mode on / off              |
| C-g / Esc                       | Cancel selection or active prompt   |
| C-h / Backspace                 | Delete backward                     |
| C-d / Del                       | Delete forward                      |
| M-w                             | Copy the selection                  |
| C-w                             | Cut the selection                   |
| C-y                             | Paste                               |
| C-s                             | Search (regex)                      |
| C-x C-s                         | Save                                |
| C-x C-c                         | Quit                                |
| C-x g                           | Go to line                          |
| C-x l                           | Last line visible on the screen     |
| C-x 2                           | Split above / below                 |
| C-x 3                           | Split side by side                  |
| C-x o / C-x O                   | Next / previous pane                |
| C-x ] / C-x [                   | Grow / shrink pane height           |
| C-x } / C-x {                   | Grow / shrink pane width            |
| C-x 0                           | Close the current pane              |
| C-x 1                           | Keep only the current pane          |
| C-x t                           | Toggle the file manager             |
| C-x f                           | Focus file manager / editor         |

Arrows, Ctrl-Left/Right, Home/End, PgUp/PgDn and Ctrl-Home/End remain
available as navigation aliases.

Press **C-x** to open a centered modal listing the available suffixes.
Press the displayed suffix to run its command, or C-g / Esc to cancel.
Unknown keys stay in the modal and never insert text. The menu is generated
from the command bindings, so there is no separate help page.
When a command also has direct bindings, its row includes them in brackets,
for example `p [C-p, Up] - Move up`: `p` is the suffix after C-x, while C-p
and Up run the same action directly.
Opposite commands share one aligned row, such as `} / { - Grow / Shrink width`.
When only one direction is available, the row shows that command alone.
A blank row separates standard commands from commands available for the current
layout, including split creation, navigation, resizing and closing.

Splits share the same document, with independent cursors, selections and scroll
positions. Up to **four panes total** can be combined in horizontal and vertical
layouts. The focused pane has a blue header marked `*`; closing a pane preserves
the document and its unsaved changes while another pane still shows it.
To show the current buffer in another pane, use C-x 2 or C-x 3 in that pane:
the new view refers to the same text, so edits immediately appear in every view.
To display another open file in the selected pane, select that file in the
file manager: existing visible documents are reused, including their unsaved edits.

The file manager opens on the left with **C-x t**, rooted at the directory where
adm was launched. It occupies a sidebar and does not count toward the four-pane
limit. **C-x f** opens it if needed and toggles keyboard focus between its tree
and the editor. The selected editor pane stays active while browsing; Enter opens
a file in that pane and returns keyboard focus to the editor. The tree lists
directories first, then files, alphabetically; hidden entries are included.

In the tree, Up/Down or C-p/C-n select entries; Right/C-f expands a directory or
enters its children; Left/C-b collapses it or selects its parent. Enter toggles
a directory or opens a file. Home/End, M-</M-> and page movement also work.
C-g / Esc returns to the selected pane. Normal text keys do not edit the buffer
while the tree has focus. Directories are read when expanded, so collapse and
expand refresh their contents. Binary files containing NUL bytes are rejected.
Replacing the last view of an unsaved buffer asks for `y`; any other key cancels
and keeps that buffer. A failed open preserves the current buffer.

Pane navigation and closing commands appear only while multiple panes exist.
Split and resize commands appear when the requested action fits: panels have
minimum sizes, and resizing adjusts the nearest divider on that axis. Height
changes by one row and width by two columns. If the menu is taller than the
terminal, Up/Down or PgUp/PgDn scroll its list; command keys still work directly.

Terminal resizing preserves the divider proportions. If the terminal cannot fit
the layout, only the focused pane is shown temporarily; C-x o / C-x O can still
switch panes. Enlarging the terminal restores the full layout. Below 30 columns
or five rows the file manager is temporarily hidden and focus returns to the
editor; enlarging the terminal restores the sidebar.

If any visible buffer has unsaved changes, C-x C-c asks for confirmation:
press `y` to quit without saving. Press `n` or any other key to cancel. The
answer is consumed: it does not insert text or run another command.
This is one confirmation for the whole editor: `y` discards every unsaved buffer,
while any other key cancels the entire exit. It does not save any file. C-x C-s
saves only the buffer in the focused pane.

A buffer stays open only while at least one pane refers to it. Closing its last
view releases its text immediately; there is no background buffer list. Closing
the last view of an unsaved buffer, or keeping only one pane when that would
discard other unsaved buffers, uses the same `y` confirmation. Closing one of
several views of a shared buffer needs no confirmation, because the buffer remains
in another pane. To close the sole remaining pane and exit, use C-x C-c.

To select text press C-SPC and move the cursor: the movement keys extend the
selection until you copy, cut, type over it, or back out with C-g / Esc or
C-SPC again. While selection mode is on the bottom bar shows a blue SELECT badge.

The bottom bar also counts the file: its lines, its characters, and while a
selection is active, the characters it covers. Characters are what you would
count by hand — an accented letter or an emoji is one character, however many
bytes it takes.

C-x g opens a GOTO prompt on the bottom bar: the view follows the line
number as you type it. Enter stays there, C-g / Esc goes back to where you were,
and a number past the end of the file stops at the last line.

C-s opens a search prompt on the bottom bar. The search is incremental:
as you type, the first match after the cursor is highlighted. Enter closes
the prompt and leaves the cursor on the match, C-g / Esc goes back to where
you started, Down (or C-s again) jumps to the next match and Up (or C-r) to
the previous one, wrapping around the ends of the file.

The query is a regular expression:

| Pattern         | Matches                                          |
|-----------------|--------------------------------------------------|
| `c`             | the character itself (UTF-8 aware)               |
| `.`             | any single character                             |
| `^` / `$`       | start / end of the line                          |
| `[abc]` `[a-z]` | a character class, `[^...]` to negate it         |
| `\d` `\w` `\s`  | digit, word character, whitespace (`\D \W \S` the opposites) |
| `\c`            | the literal character `c`, for when `c` is special |
| `*` `+` `?`     | zero or more, one or more, zero or one of the last element |

The engine is built in, so it works the same on every platform; it has no
groups, alternation or back references. A red bar means the pattern is
malformed or matches nothing.


The clipboard is the system one. On POSIX it goes through whichever of
`wl-copy`, `xclip`, `xsel` or `pbcopy` is installed, falling back to the
OSC 52 terminal escape (which works over ssh); on Windows it is the native
clipboard. When none of those can be reached the editor still copies and
pastes within itself.

## Architecture

`document.h` owns the buffer, filename, dirty state and count of pane references.
`document.c` releases text when that count reaches zero. `view.h` contains the
cursor, selection and scrolling for a retained document. `editor.h` joins the
document, workspace and focused view without duplicating text between panes.
Documents opened from the tree own their paths and are freed with their last view;
the pane collection is the only registry for reusing documents.

`layout.c` owns the split tree, focus, size constraints and pane lifecycle.
Each branch divides its rectangle using a saved proportion, so nested horizontal
and vertical splits follow the same rules. The bounded tree matches the four-pane
limit and requires no dynamic allocation. Buffer range-change notifications rebase
other views through `view.c`; completed edits clamp positions to UTF-8 grapheme
boundaries while preserving the preferred column for vertical movement.

`screen.c` renders rectangles supplied by the layout. `command_menu.c` handles
the centered menu and its scroll position. Command labels, direct shortcuts and
visibility predicates live in the dispatch registry: adding a contextual command
does not require another manually maintained menu map.

`file_tree.c` owns a flattened tree of visible entries, independent of the editor.
Expanding scans just that directory; collapsing releases its descendants.
`file_manager.c` handles sidebar visibility, keyboard focus and opening files in
the selected pane. Its focus does not change the layout's active view, and the
layout reserves sidebar space before arranging splits. `path.c` centralizes path
joining and resolution. Global commands preserve keyboard focus, while editor
commands return focus to the selected pane before executing.
