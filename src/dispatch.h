#ifndef DISPATCH_H
#define DISPATCH_H

#include "buffer.h"

// A key is either a Unicode code point (0 to 0x10FFFF) or one of the special
// keys below, which start past the end of the Unicode range so that they can
// never collide with a typed character.
#define KEY_SPECIAL 0x110000

enum {
  KEY_LEFT             = 0x110000,
  KEY_RIGHT            = 0x110001,
  KEY_UP               = 0x110002,
  KEY_DOWN             = 0x110003,
  KEY_CTRL_LEFT        = 0x110004,
  KEY_CTRL_RIGHT       = 0x110005,
  KEY_DELETE           = 0x110006,
  KEY_PGUP             = 0x110007,
  KEY_PGDOWN           = 0x110008,
  KEY_HOME             = 0x110009,
  KEY_END              = 0x11000A,
  KEY_CTRL_HOME        = 0x11000B,
  KEY_CTRL_END         = 0x11000C
};

// Backspace, which terminals send as the ASCII DEL byte rather than as an
// escape sequence. It is a normal code point, so it lives outside the enum.
#define KEY_BACKSPACE 127

// Control-key code.
#define CTRL(k) ((k) & 0x1f)

struct abuf; // defined in screen.h

// Shared editor state.
typedef struct editor {
  buffer buf;
  const char *filename;
  int cx, cy;         // cursor in file coordinates (column, row)
  int selx, sely;     // selection anchor, the fixed end (cx/cy is the moving one)
  int sel_active;     // 1 if there is an active selection
  int sel_mode;       // 1 while selection mode is on: movement extends the selection
  int sticky;         // column vertical movement aims for, in screen columns
  int rowoff, coloff; // vertical/horizontal scroll offsets
  int rows, cols;     // terminal size
  int dirty;          // 1 if there is unsaved changes
  int quit_pending;   // 1 while a quit is waiting to be confirmed
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

