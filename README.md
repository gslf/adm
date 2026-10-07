# ][adm

An opinionated, batteries-included text editor. It supports themes and plugins, but you probably won’t need them. Out of the box, ADM looks great and comes with a built-in file explorer, Git manager, a powerful integrated search tool (blazing fast), syntax highlighting, and LSP support.

![adm ScreenShot](res/ss.png)

## Overview

- **Emacs inspired controls**
- **Splits & Tabs**
- **File Explorer**
- **Git Manager**
- **Editing tools**
- **Syntax highlighting and LSP**
- **Configurable themes**
- **Plugins**

## Build and run

```sh
make
./adm [file]
make test
```

Requires a C11 compiler and Git 2.23+ for Git features. Tests require Python 3 and Git, using temporary repositories and local remotes; POSIX tests also cover the terminal UI. Clean with `make clean`.

Read the [user guide and keyboard shortcuts](HELP.md) for editor usage.
