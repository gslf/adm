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
  // Unsaved work is never thrown away on a single keystroke. The first press
  // only puts the question up in the bottom bar; the second one goes through.
  // dispatch_key takes the question back down as soon as any other key is
  // pressed, so an accidental Ctrl-Q cannot sit there waiting to be armed.
  if (e->dirty && !e->quit_pending) {
    e->quit_pending = 1;
    return;
  }

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

// Remove the active selection, if there is one. Returns 1 if it removed
// something, so callers can tell an erasing keystroke is already done.
static int drop_selection(editor *e) {
  if (!e->sel_active)
    return 0;

  selection_delete(e);
  e->dirty = 1;
  dispatch_change(e);
  return 1;
}

// Command - DELETE forward
static void cmd_delete(editor *e) {
  // Delete a selection
  if (drop_selection(e))
    return;

  char *line = buffer_line(&e->buf, e->cy);
  int len = line ? (int)strlen(line) : 0;

  // Delete a single character
  // Remove the whole cluster under the cursor, however many bytes that is
  if (e->cx < len) {
    int end = grapheme_next(line, e->cx);
    for (int i = end - e->cx; i > 0; i--)
      buffer_delete_char(&e->buf, e->cy, e->cx);
    e->dirty = 1;
    dispatch_change(e);

  // At the line end, pull the next line up
  } else if (e->cy < e->buf.nlines - 1) {
    if (buffer_join_line(&e->buf, e->cy) == 0) {
      e->dirty = 1;
      dispatch_change(e);
    }
  }
}

// Command - DELETE backward
static void cmd_backspace(editor *e) {
  // Delete a selection
  if (drop_selection(e))
    return;

  // Delete a single character
  // Remove the whole cluster before the cursor, however many bytes that is
  if (e->cx > 0) {
    char *line = buffer_line(&e->buf, e->cy);
    int start = line ? grapheme_prev(line, e->cx) : e->cx - 1;
    for (int i = e->cx - start; i > 0; i--)
      buffer_delete_char(&e->buf, e->cy, start);
    e->cx = start;
    e->dirty = 1;
    cursor_mark_column(e);
    dispatch_change(e);

  // At column 0, merge the line with the previous one
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
  dispatch_bind(KEY_BACKSPACE,        cmd_backspace);
  dispatch_bind(KEY_PGUP,             cursor_page_up);
  dispatch_bind(KEY_PGDOWN,           cursor_page_down);
  dispatch_bind(KEY_HOME,             cursor_home);
  dispatch_bind(CTRL('s'), cmd_save);
  dispatch_bind(CTRL('q'), cmd_quit);

  // Module init hooks.
  for (int i = 0; i < nmods; i++)
    if (mods[i]->init)
      mods[i]->init(e);
}

// Default text input
static void edit_key(editor *e, int key) {
  int text = (key >= 32 && key < KEY_SPECIAL && key != KEY_BACKSPACE);

  // Not an editing key
  if (key != '\r' && key != '\n' && !text)
    return;

  // Typing over a selection replaces it.
  drop_selection(e);

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

  // Characters to add, encoded back into UTF-8 bytes
  char seq[4];
  int n = utf8_encode(key, seq);
  if (n == 0)
    return; // nothing this key could turn into

  for (int i = 0; i < n; i++)
    if (buffer_insert_char(&e->buf, e->cy, e->cx + i, seq[i]) != 0) {
      // Take back the bytes that did go in: half a sequence is not a
      // character, and it would break every offset on the line.
      while (i-- > 0)
        buffer_delete_char(&e->buf, e->cy, e->cx);
      return;
    }
  e->cx += n;
  e->dirty = 1;
  cursor_mark_column(e);
  dispatch_change(e);
}

void dispatch_key(editor *e, int key) {
  // The command bound to the key, if any.
  int slot = key_slot(key);
  command cmd = (slot >= 0) ? cmds[slot] : NULL;

  // A pending quit is a question waiting for an answer, and only another quit
  // answers it. Any other key means the user carried on working, so the
  // question comes down. Clearing it here, before anything runs, is what lets
  // cmd_quit tell a first press from a second one.
  if (cmd != cmd_quit)
    e->quit_pending = 0;

  // Modules first: if one consumes the key, stop the chain.
  for (int i = 0; i < nmods; i++)
    if (mods[i]->on_key && mods[i]->on_key(e, key))
      return;

  if (cmd) {
    cmd(e);
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

// Modifier keys held down together with a special key. The terminal reports
// them as one number, biased by one, so "1;6A" carries 6 and the mask is 5,
// which reads as shift and ctrl together.
enum {
  MOD_SHIFT = 1,
  MOD_ALT   = 2,
  MOD_CTRL  = 4
};

// Read the parameter bytes of an escape sequence into par, which is left NUL
// terminated. Returns the final byte that ended the sequence, since that is
// what tells the caller which sequence it was, or 0 if it never arrived.
static char read_escape_params(char *par, int size) {
  int n = 0;

  for (;;) {
    char c;
    if (READ(0, &c, 1) != 1)
      return 0; // the terminal stopped mid sequence

    // Anything from '@' to '~' is the final byte and ends the sequence.
    if (c >= '@' && c <= '~') {
      par[n] = '\0';
      return c;
    }

    // Parameters too long for the buffer are dropped but still consumed, so
    // that the next read starts on a clean byte.
    if (n < size - 1)
      par[n++] = c;
  }
}

// The mask of modifiers carried by a parameter string like "1;6", or 0 when
// the terminal reported none.
static int escape_modifiers(const char *par) {
  const char *semicolon = strchr(par, ';');
  if (!semicolon)
    return 0; // no modifier in the sequence at all

  int reported = atoi(semicolon + 1);
  if (reported < 1)
    return 0; // malformed, read it as no modifier

  return reported - 1; // undo the bias
}

// Keys that all end with '~' and tell themselves apart by a leading number.
// Returns 0 for a number the editor has no key for.
static int numbered_key(const char *par) {
  switch (atoi(par)) {
    case 1:  return KEY_HOME;
    case 3:  return KEY_DELETE;
    case 5:  return KEY_PGUP;
    case 6:  return KEY_PGDOWN;
    case 7:  return KEY_HOME; // some terminals send 7 where others send 1
    default: return 0;
  }
}

// Turn a completed escape sequence into a key. Returns 0 if it is a valid
// sequence that the editor simply has no key for.
static int escape_to_key(char final, const char *par) {
  int mod = escape_modifiers(par);

  switch (final) {
    case 'A':
      if (mod == (MOD_CTRL | MOD_SHIFT))
        return KEY_CTRL_SHIFT_UP;
      return KEY_UP;

    case 'B':
      if (mod == (MOD_CTRL | MOD_SHIFT))
        return KEY_CTRL_SHIFT_DOWN;
      return KEY_DOWN;

    case 'C':
      if (mod == (MOD_CTRL | MOD_SHIFT))
        return KEY_CTRL_SHIFT_RIGHT;
      if (mod == MOD_CTRL)
        return KEY_CTRL_RIGHT;
      return KEY_RIGHT;

    case 'D':
      if (mod == (MOD_CTRL | MOD_SHIFT))
        return KEY_CTRL_SHIFT_LEFT;
      if (mod == MOD_CTRL)
        return KEY_CTRL_LEFT;
      return KEY_LEFT;

    case 'H':
      return KEY_HOME;

    case '~':
      return numbered_key(par);

    default:
      return 0;
  }
}

int read_key(void) {
  char c;
  if (READ(0, &c, 1) != 1)
    return 0; // no input (timeout)

  // The key is not an escape sequence
  if (c != '\x1b') {
    // Cast a signed char in an unsigned char
    unsigned char b = (unsigned char)c;
    if (b < 0x80)
      return b;

    // UTF-8 lead byte management
    int len;
    if ((b & 0xE0) == 0xC0)       // 110xxxxx
      len = 2;
    else if ((b & 0xF0) == 0xE0)  // 1110xxxx
      len = 3;
    else if ((b & 0xF8) == 0xF0)  // 11110xxx
      len = 4;
    else                          // not a lead byte, take it as it is
      len = 1;

    // Pull in the continuation bytes and return the code point.
    char seq[4] = {(char)b};
    for (int i = 1; i < len; i++)
      if (READ(0, &seq[i], 1) != 1)
        return b; // truncated, keep the byte rather than dropping it

    int cp;
    if (utf8_decode(seq, &cp) != len)
      return b; // malformed, the decode is not possible
    return cp;
  }

  // The key is an escape sequence: ESC, then the byte that introduces it,
  // then the parameters, then the final byte.
  char introducer;
  if (READ(0, &introducer, 1) != 1)
    return '\x1b'; // ESC pressed on its own

  // CSI ("\x1b[") or SS3 ("\x1bO"), which some terminals use for the home key
  if (introducer != '[' && introducer != 'O')
    return '\x1b';

  char par[8];
  char final = read_escape_params(par, (int)sizeof par);
  if (final == 0)
    return '\x1b'; // incomplete sequence

  int key = escape_to_key(final, par);
  if (key == 0)
    return '\x1b'; // a sequence with no key behind it

  return key;
}
