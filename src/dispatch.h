#ifndef DISPATCH_H
#define DISPATCH_H

#include "buffer.h"

// Special keys (values outside the ASCII range).
enum {
  KEY_LEFT = 1000,
  KEY_RIGHT,
  KEY_UP,
  KEY_DOWN
};

// Control-key code.
#define CTRL(k) ((k) & 0x1f)

struct abuf; // defined in screen.h

// Shared editor state.
typedef struct editor {
  buffer buf;
  const char *filename;
  int cx, cy;         // cursor in file coordinates (column, row)
  int rowoff, coloff; // vertical/horizontal scroll offsets
  int rows, cols;     // terminal size
  int dirty;          // unsaved changes
  int running;        // main loop flag
} editor;

// Command type
typedef void (*command)(editor *e);

// Module interface.
typedef struct module {
  const char *name;
  void (*init)(editor *e);
  int  (*on_key)(editor *e, int key);   // returns 1 if the key is consumed
  void (*on_draw)(editor *e, struct abuf *ab);
  void (*on_change)(editor *e);
  void (*shutdown)(editor *e);
} module;

// Command table and module registry.
void dispatch_bind(int key, command cmd); // bind a key to a command
void dispatch_register(module *m);         // register a module
void dispatch_init(editor *e);             // set up default bindings, init modules
void dispatch_key(editor *e, int key);     // on_key chain, then bound command
void dispatch_draw(editor *e, struct abuf *ab); // on_draw chain
void dispatch_change(editor *e);           // on_change chain
void dispatch_shutdown(editor *e);         // call shutdown of every module

// Read and decode one key
int read_key(void);

#endif

