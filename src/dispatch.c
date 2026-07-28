#include "dispatch.h"
#include "cursor.h"
#include "fileio.h"
#include "utf8.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define READ _read
#else
#include <unistd.h>
#define READ read
#endif

#define KEY_SLOTS 144 // 0 to 127 for ASCII and control, then 16 special keys
#define MAX_MODULES 16

static command cmds[KEY_SLOTS];
static module *mods[MAX_MODULES];
static int nmods = 0;

// Map a key code to its slot in the command table, or -1 if it has none.
// Everything above 127 that is not a special key is text, never a command.
static int key_slot(int key) {
  if (key >= 0 && key < 128)
    return key;
  if (key >= KEY_SPECIAL && key < KEY_SPECIAL + (KEY_SLOTS - 128))
    return 128 + (key - KEY_SPECIAL);
  return -1;
}

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

// Command - DELETE forward
static void cmd_delete(editor *e) {
  // With a selection active, Delete just removes it.
  if (e->sel_active) {
    selection_delete(e);
    e->dirty = 1;
    dispatch_change(e);
    return;
  }

  char *line = buffer_line(&e->buf, e->cy);
  int len = line ? (int)strlen(line) : 0;

  // Remove the whole cluster under the cursor, however many bytes that is.
  if (e->cx < len) {
    int end = grapheme_next(line, e->cx);
    for (int i = end - e->cx; i > 0; i--)
      buffer_delete_char(&e->buf, e->cy, e->cx);
    e->dirty = 1;
    dispatch_change(e);

  // At the line end, pull the next line up instead.
  } else if (e->cy < e->buf.nlines - 1) {
    if (buffer_join_line(&e->buf, e->cy) == 0) {
      e->dirty = 1;
      dispatch_change(e);
    }
  }
}

void dispatch_bind(int key, command cmd) {
  int slot = key_slot(key);
  if (slot >= 0)
    cmds[slot] = cmd;
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
  dispatch_bind(KEY_CTRL_SHIFT_UP,    cursor_select_up);
  dispatch_bind(KEY_CTRL_SHIFT_DOWN,  cursor_select_down);
  dispatch_bind(KEY_CTRL_SHIFT_LEFT,  cursor_select_left);
  dispatch_bind(KEY_CTRL_SHIFT_RIGHT, cursor_select_right);
  dispatch_bind(KEY_CTRL_LEFT,        cursor_word_left);
  dispatch_bind(KEY_CTRL_RIGHT,       cursor_word_right);
  dispatch_bind(KEY_DELETE,           cmd_delete);
  dispatch_bind(CTRL('s'), cmd_save);
  dispatch_bind(CTRL('q'), cmd_quit);

  // Module init hooks.
  for (int i = 0; i < nmods; i++)
    if (mods[i]->init)
      mods[i]->init(e);
}

// Default text input
static void edit_key(editor *e, int key) {
  int text = (key >= 32 && key < KEY_SPECIAL && key != 127);
  if (key != '\r' && key != '\n' && key != 127 && !text)
    return; // not an editing key, leave the selection alone

  // Typing over a selection replaces it, backspace just removes it.
  if (e->sel_active) {
    selection_delete(e);
    e->dirty = 1;
    dispatch_change(e);
    if (key == 127)
      return;
  }

  // Enter
  if (key == '\r' || key == '\n') {
    if (buffer_insert_newline(&e->buf, e->cy, e->cx) == 0) {
      e->cy++;
      e->cx = 0;
      e->dirty = 1;
      cursor_mark_column(e);
      dispatch_change(e);
    }
    return;
  }

  // Backspace
  if (key == 127) {

    // Delete a whole grapheme cluster, however many bytes that is
    if (e->cx > 0) {
      char *line = buffer_line(&e->buf, e->cy);
      int start = line ? grapheme_prev(line, e->cx) : e->cx - 1;
      for (int i = e->cx - start; i > 0; i--)
        buffer_delete_char(&e->buf, e->cy, start);
      e->cx = start;
      e->dirty = 1;
      cursor_mark_column(e);
      dispatch_change(e);

    // At column 0 merge the line with the previous one
    } else if (e->cy > 0) {
      char *prev = buffer_line(&e->buf, e->cy - 1);
      int prevlen = prev ? (int)strlen(prev) : 0;
      if (buffer_join_line(&e->buf, e->cy - 1) == 0) {
        e->cy--;
        e->cx = prevlen;
        e->dirty = 1;
        cursor_mark_column(e);
        dispatch_change(e);
      }
    }
    return;
  }


  // Characters to add, encoded back into UTF-8 bytes
  if (text) {
    char seq[4];
    int n = utf8_encode(key, seq);
    for (int i = 0; i < n; i++)
      if (buffer_insert_char(&e->buf, e->cy, e->cx + i, seq[i]) != 0)
        return;
    e->cx += n;
    e->dirty = 1;
    cursor_mark_column(e);
    dispatch_change(e);
  }
}

void dispatch_key(editor *e, int key) {
  // Modules first: if one consumes the key, stop the chain.
  for (int i = 0; i < nmods; i++)
    if (mods[i]->on_key && mods[i]->on_key(e, key))
      return;

  // Then the command bound to the key, if any.
  int slot = key_slot(key);
  if (slot >= 0 && cmds[slot]) {
    cmds[slot](e);
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

  if (c != '\x1b') {
    unsigned char b = (unsigned char)c;
    if (b < 0x80)
      return b;

    // UTF-8 lead byte: pull in the continuation bytes and return the code point.
    int len = (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3
            : (b & 0xF8) == 0xF0 ? 4 : 1;
    char seq[4] = {(char)b};
    for (int i = 1; i < len; i++)
      if (READ(0, &seq[i], 1) != 1)
        return b; // truncated, keep the byte rather than dropping it

    int cp;
    utf8_decode(seq, &cp);
    return cp;
  }

  // Escape sequence: expect the CSI introducer.
  if (READ(0, &c, 1) != 1 || c != '[')
    return '\x1b';

  // Collect the parameter bytes, stop on the final byte ('@' to '~').
  char par[8];
  int n = 0;
  for (;;) {
    if (READ(0, &c, 1) != 1)
      return '\x1b'; // incomplete sequence
    if (c >= '@' && c <= '~')
      break;
    if (n < (int)sizeof(par) - 1)
      par[n++] = c;
  }
  par[n] = '\0';

  // Arrows come bare, or as "1;<mod>" where mod is 1 plus a bit mask:
  // 1 shift, 2 alt, 4 ctrl. So 6 is ctrl+shift, the selection modifier.
  int mod = 0;
  for (int k = 0; par[k]; k++)
    if (par[k] == ';') {
      mod = atoi(par + k + 1);
      break;
    }
  switch (c) {
    case 'A':
      return mod == 6 ? KEY_CTRL_SHIFT_UP : KEY_UP;
    case 'B':
      return mod == 6 ? KEY_CTRL_SHIFT_DOWN : KEY_DOWN;
    case 'C':
      if (mod == 6) return KEY_CTRL_SHIFT_RIGHT;
      if (mod == 5) return KEY_CTRL_RIGHT;
      return KEY_RIGHT;
    case 'D':
      if (mod == 6) return KEY_CTRL_SHIFT_LEFT;
      if (mod == 5) return KEY_CTRL_LEFT;
      return KEY_LEFT;
    case '~':
      // Keys that report as a number: 3 is Delete.
      if (atoi(par) == 3)
        return KEY_DELETE;
      break;
  }

  return '\x1b';
}
