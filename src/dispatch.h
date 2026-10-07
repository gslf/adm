#ifndef DISPATCH_H
#define DISPATCH_H

#include "editor.h"
#include <stddef.h>

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
  KEY_CTRL_END         = 0x11000C,
  KEY_UNKNOWN          = 0x11000D
};

// Backspace, which terminals send as the ASCII DEL byte rather than as an
// escape sequence. It is a normal code point, so it lives outside the enum.
#define KEY_BACKSPACE 127
#define KEY_NONE (-1)

// Meta is sent by terminals as Escape followed by the character (Alt-key).
#define KEY_META 0x120000
#define META(k) (KEY_META + (k))

// Control-key code.
#define CTRL(k) ((k) & 0x1f)

struct abuf; // defined in screen.h

// Command type
typedef editor_action command;
typedef int (*command_condition)(const editor *e);

typedef struct command_binding {
  int key;
  command cmd;
  const char *keys, *label;
  command_condition when;
  int pair_key, pair_primary;
  const char *pair_label;
  int preserve_focus;
} command_binding;

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
void dispatch_bind_prefix(int key, command cmd, const char *keys,
                          const char *label); // bind a suffix after C-x
void dispatch_bind_prefix_global(int key, command cmd, const char *keys,
                                 const char *label);
void dispatch_bind_prefix_global_when(int key, command cmd, const char *keys,
                                      const char *label, command_condition when);
void dispatch_bind_prefix_when(int key, command cmd, const char *keys,
                               const char *label, command_condition when);
void dispatch_pair_prefix(int first_key, int second_key, const char *label);
int dispatch_prefix_count(const editor *e);
const command_binding *dispatch_prefix_at(const editor *e, int index);
const command_binding *dispatch_prefix_find(const editor *e, int key);
void dispatch_direct_bindings(command cmd, char *keys, size_t size);
void dispatch_register(module *m);         // register a module
void dispatch_init(editor *e);             // set up default bindings, init modules
void dispatch_key(editor *e, int key);     // on_key chain, then bound command
void dispatch_draw(editor *e, struct abuf *ab); // on_draw chain
void dispatch_change(editor *e);           // on_change chain
void dispatch_tick(editor *e);
void dispatch_shutdown(editor *e);         // call shutdown of every module
void dispatch_confirm(editor *e, command action, const char *prompt);
void dispatch_confirm_with_cancel(editor *e, command action, command cancel,
                                  const char *prompt);

// Read and decode one key; KEY_NONE means no input, 0 is C-SPC.
int read_key(void);

#endif
