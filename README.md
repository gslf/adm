# ][adm

A small terminal text editor for POSIX and Windows, built with C11.

![adm ScreenShot](res/ss.png)

## Overview

- **Emacs inspired controls**
- **Split panes** 
- **Sidebar** 
- **File Explorer**
- **Git Manager** 
- **Editing tools**

## Build and run

```sh
make
./adm [file]
make test
```

Requires a C11 compiler and Git 2.23+ for Git features. Tests require Python 3 and Git, using temporary repositories and local remotes; POSIX tests also cover the terminal UI. Missing files start empty and are created on save. Without an argument, adm opens an unnamed buffer. Clean with `make clean`.

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


## Sidebar

The sidebar shows either the File Explorer or Git Manager. 

In the **File Explorer** the tree starts in the directory where adm was launched. Use arrows or C-p/n/b/f to navigate, expand and collapse folders. Enter toggles folders, opening a file with Enter targets the selected pane and returns focus to the editor. 

In the **Git Manager**, Enter opens the selected staged or unstaged diff in the active pane, in read-only mode, **o** opens the file for editing. Diffs show removed lines on dark red, added lines on dark green and unchanged context on grey. Git uses saved files, so commits include only staged changes. 
