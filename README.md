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

## Usage

```
adm [file]
```

Opens `file` for editing. If it does not exist yet, the buffer starts empty and
the file is created on save. With no argument you get an empty buffer.


| Key                 | Action                                            |
|---------------------|---------------------------------------------------|
| Arrows              | Move the cursor                                   |
| Ctrl-Left / Right   | Move by word                                      |
| Home / End          | Start / end of the line                           |
| PgUp / PgDn         | One screen up / down                              |
| Ctrl-T / Ctrl-E     | First / last line of the file (also Ctrl-Home / Ctrl-End) |
| Ctrl-L              | Last line visible on the screen                   |
| Ctrl-G              | Go to line                                        |
| Ctrl-B              | Selection mode on / off                           |
| Esc                 | Cancel the selection                              |
| Backspace / Del     | Delete                                            |
| Ctrl-C              | Copy the selection                                |
| Ctrl-X              | Cut the selection                                 |
| Ctrl-V              | Paste                                             |
| Ctrl-F              | Search (regex)                                    |
| Ctrl-S              | Save                                              |
| Ctrl-Q              | Quit                                              |
| Ctrl-/              | Help page with all the bindings                   |

To select text press Ctrl-B and move the cursor: the movement keys extend the
selection until you copy, cut, type over it, or back out with Esc or Ctrl-B
again. While selection mode is on the bottom bar shows a blue SELECT badge.

The bottom bar also counts the file: its lines, its characters, and while a
selection is active, the characters it covers. Characters are what you would
count by hand — an accented letter or an emoji is one character, however many
bytes it takes.

Ctrl-G opens a GOTO prompt on the bottom bar: the view follows the line
number as you type it. Enter stays there, Esc goes back to where you were,
and a number past the end of the file stops at the last line.

Ctrl-/ (the terminal reports it as Ctrl-_, so that works too) covers the
screen with the full key map; any key puts the text back.


Ctrl-F opens a search prompt on the bottom bar. The search is incremental:
as you type, the first match after the cursor is highlighted. Enter closes
the prompt and leaves the cursor on the match, Esc goes back to where you
started, Down (or Ctrl-F again) jumps to the next match and Up to the
previous one, wrapping around the ends of the file.

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
