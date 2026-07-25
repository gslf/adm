#include "dispatch.h"
#include "cursor.h"
#include "fileio.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define READ _read
#else
#include <unistd.h>
#define READ read
#endif

#define MAX_KEY 1200
#define MAX_MODULES 16

static command cmds[MAX_KEY];
static module *mods[MAX_MODULES];
static int nmods = 0;

// Command - QUIT
static void cmd_quit(editor *e) {
  e->running = 0;
}

// Command - SAVE
static void cmd_save(editor *e) {
  if (!e->filename)
    return; 

  // Buffer serializatiob
  size_t total = 0;
  for (block *k = e->buf.head; k; k = k->next)
    for (int i = 0; i < k->count; i++)
      total += strlen(k->lines[i]) + 1;

  char *text = malloc(total + 1);
  if (!text)
    return;

  size_t pos = 0;
  for (block *k = e->buf.head; k; k = k->next)
    for (int i = 0; i < k->count; i++) {
      size_t len = strlen(k->lines[i]);
      memcpy(text + pos, k->lines[i], len);
      pos += len;
      text[pos++] = '\n';
    }
  text[pos] = '\0';

  if (file_write(e->filename, text) == 0)
    e->dirty = 0;

  free(text);
}

void dispatch_bind(int key, command cmd) {
  if (key >= 0 && key < MAX_KEY)
    cmds[key] = cmd;
}

void dispatch_register(module *m) {
  if (nmods < MAX_MODULES)
    mods[nmods++] = m;
}

void dispatch_init(editor *e) {
  // Built-in key bindings.
  dispatch_bind(KEY_UP,    cursor_up);
  dispatch_bind(KEY_DOWN,  cursor_down);
  dispatch_bind(KEY_LEFT,  cursor_left);
  dispatch_bind(KEY_RIGHT, cursor_right);
  dispatch_bind(CTRL('s'), cmd_save);
  dispatch_bind(CTRL('q'), cmd_quit);

  // Module init hooks.
  for (int i = 0; i < nmods; i++)
    if (mods[i]->init)
      mods[i]->init(e);
}

// Default text input
static void edit_key(editor *e, int key) {

  // Enter
  if (key == '\r' || key == '\n') {
    if (buffer_insert_newline(&e->buf, e->cy, e->cx) == 0) {
      e->cy++;
      e->cx = 0;
      e->dirty = 1;
      dispatch_change(e);
    }
    return;
  }

  // Backspace
  if (key == 127) {

    // Delete a character
    if (e->cx > 0) {
      if (buffer_delete_char(&e->buf, e->cy, e->cx - 1) == 0) {
        e->cx--;
        e->dirty = 1;
        dispatch_change(e);
      }

    // At column 0 merge the line with the previous one
    } else if (e->cy > 0) {
      char *prev = buffer_line(&e->buf, e->cy - 1);
      int prevlen = prev ? (int)strlen(prev) : 0;
      if (buffer_join_line(&e->buf, e->cy - 1) == 0) {
        e->cy--;
        e->cx = prevlen;
        e->dirty = 1;
        dispatch_change(e);
      }
    }
    return;
  }


  // Characters to add
  if (key >= 32 && key < 127) {
    if (buffer_insert_char(&e->buf, e->cy, e->cx, (char)key) == 0) {
      e->cx++;
      e->dirty = 1;
      dispatch_change(e);
    }
  }
}

void dispatch_key(editor *e, int key) {
  // Modules first: if one consumes the key, stop the chain.
  for (int i = 0; i < nmods; i++)
    if (mods[i]->on_key && mods[i]->on_key(e, key))
      return;

  // Then the command bound to the key, if any.
  if (key >= 0 && key < MAX_KEY && cmds[key]) {
    cmds[key](e);
    return;
  }

  // Otherwise treat it as text input.
  edit_key(e, key);
}

void dispatch_draw(editor *e, struct abuf *ab) {
  for (int i = 0; i < nmods; i++)
    if (mods[i]->on_draw)
      mods[i]->on_draw(e, ab);
}

void dispatch_change(editor *e) {
  for (int i = 0; i < nmods; i++)
    if (mods[i]->on_change)
      mods[i]->on_change(e);
}

void dispatch_shutdown(editor *e) {
  for (int i = 0; i < nmods; i++)
    if (mods[i]->shutdown)
      mods[i]->shutdown(e);
}

int read_key(void) {
  char c;
  if (READ(0, &c, 1) != 1)
    return 0; // no input (timeout)

  if (c != '\x1b')
    return (unsigned char)c;

  // Escape sequence: try to read the two following bytes.
  char seq[2];
  if (READ(0, &seq[0], 1) != 1)
    return '\x1b';
  if (READ(0, &seq[1], 1) != 1)
    return '\x1b';

  if (seq[0] == '[') {
    switch (seq[1]) {
      case 'A': return KEY_UP;
      case 'B': return KEY_DOWN;
      case 'C': return KEY_RIGHT;
      case 'D': return KEY_LEFT;
    }
  }

  return '\x1b';
}
