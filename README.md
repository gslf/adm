# ][adm

A minimal terminal text editor that does **one** thing, edit text, and does it well.

## Philosophy

][adm has a single purpose: editing text. That is the whole scope. Every line of
code exists to serve that goal, nothing else — no configuration files, no macros,
no bloat.

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

### Keys binindgs

| Key           | Action              |
|---------------|---------------------|
| Ctrl-S        | Save                |
| Ctrl-Q        | Quit                |

That's it.
