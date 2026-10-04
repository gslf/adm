# ][adm

A small terminal text editor for POSIX and Windows, built with C11.

![adm ScreenShot](res/ss.png)

## Overview

- **Emacs controls:** direct editing shortcuts and a centered **C-x Command Center** with aligned, contextual commands and direct aliases.
- **Split panes:** up to **four panes**, with horizontal/vertical splits, navigation and resizing. Shared buffers keep independent cursors and scrolling.
- **Shared sidebar:** a tree file explorer or Git panel, targeting the selected pane without counting as a split.
- **Git basics:** staged/unstaged changes, conflicts, highlighted diffs, staging, commits, pull/push, checkout and merge.
- **Visible buffers only:** closing the last view releases its buffer; unsaved changes require confirmation.
- **Editing tools:** Unicode, selections, regex search, go-to-line, clipboard and file statistics.

## Build and run

```sh
make
./adm [file]
make test
```

Requires a C11 compiler and Git 2.23+ for Git features. Tests require Python 3
and Git, using temporary repositories and local remotes; POSIX tests also cover
the terminal UI. Missing files start empty and are created on save. Without an
argument, adm opens an unnamed buffer. Clean with `make clean`.

## Controls

`C-` means Ctrl; `M-` means Alt, or Esc followed by the key.

| Key | Action |
|-----|--------|
| C-p / C-n / C-b / C-f | Move up / down / left / right |
| M-b / M-f | Previous / next word |
| C-a / C-e | Start / end of line |
| M-v / C-v | Page up / down |
| M-< / M-> | Start / end of file |
| C-SPC | Toggle selection mode |
| M-w / C-w / C-y | Copy / cut / paste |
| C-h / C-d | Delete backward / forward |
| C-s | Incremental regex search; C-s / C-r selects next / previous match |
| C-g / Esc | Cancel selection or prompt; return from sidebar to editor |
| C-x C-s / C-x C-c | Save current buffer / quit |
| C-x g / C-x l | Go to line / last visible line |
| C-x 2 / C-x 3 | Split above-below / side by side |
| C-x o / C-x O | Next / previous pane |
| C-x ] / C-x [ | Grow / shrink pane height |
| C-x } / C-x { | Grow / shrink pane width |
| C-x 0 / C-x 1 | Close current pane / keep only current pane |
| C-x t | Toggle file manager |
| C-x v | Toggle Git panel; focus it when opening |
| C-x f | Switch focus between sidebar and editor |

Terminal navigation keys remain available. Press **C-x** and a displayed suffix
to run a command; C-g / Esc cancels. Contextual commands appear when available,
and Up/Down or PgUp/PgDn scroll a menu that exceeds the terminal height.

## Files and panes

The sidebar shows either the file explorer or Git, never both. **C-x f** focuses
the current panel; if closed, it reopens the last used panel, defaulting to the
file explorer. **C-x v** opens and focuses Git. Sidebar focus preserves the
selected editor pane.

The tree starts in the directory where adm was launched. Use arrows or C-p/n/b/f
to navigate, expand and collapse folders. Enter toggles folders; opening a file
with Enter targets the selected pane and returns focus to the editor. Browsing
leaves that pane selected. To show its buffer in another pane, use C-x 2 or C-x 3.

Closing or replacing the last view of an unsaved buffer requires **y** to discard
changes; **n or any other key cancels**. Quitting checks every visible buffer and
uses one confirmation: y discards all unsaved changes. C-x C-s saves only the
selected buffer. Failed file opens preserve the current buffer.

Terminal resizing preserves the split layout; small terminals temporarily show
only the selected pane or hide the sidebar, restoring them when space returns.

## Git

In the Git panel, **Enter** opens the selected staged or unstaged diff in the
active pane, in read-only mode; **o** opens the file for editing. Use **s/u** to
stage/unstage a file, **S/U** for all changes, **c** to enter a commit message,
**p/P** for pull/push, **b** for checkout or creating a branch, **m** for merge,
and **r** to refresh. The main shortcuts are shown at the bottom of the panel.

Diffs show removed lines on dark red, added lines on dark green and unchanged
context on grey. Staged diffs compare HEAD with the index; unstaged diffs compare
the index with saved files. A changed line appears as a removal and an addition.

Git uses saved files; commits include only staged changes. Save modified buffers
before staging them or changing the worktree. Operations run asynchronously;
worktree updates briefly pause editing and reload clean open files. Pull uses
fast-forward only. Conflicts appear in their own section: edit, save and stage
the resolved files, then commit. Remotes and credentials use your Git configuration.

## Architecture

Documents own shared text; views own cursor and scroll state. Split layout, tree
model, file-manager controls and rendering are separate modules. The command
registry supplies menu labels, aliases and visibility rules. A single sidebar
state manages panel visibility, focus and restoration. Git status parsing,
process execution, diff classification, controls and rendering are separate modules.
