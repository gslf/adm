#include "dispatch.h"
#include "cursor.h"
#include "fileio.h"
#include "utf8.h"
#include "screen.h"
#include "command_menu.h"

#include <stdio.h>

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define READ _read
#else
#include <unistd.h>
#define READ read
#endif

#define KEY_SLOTS 272 // ASCII/control, 16 special keys, 128 Meta characters
#define MAX_MODULES 16

static command cmds[KEY_SLOTS];
static command_binding prefix_entries[KEY_SLOTS];
static int nprefix;
static module *mods[MAX_MODULES];
static int nmods = 0;

// Map a key code to its slot in the command table, or -1 if it has none.
// Everything above 127 that is not a special key is text, never a command.
static int key_slot(int key) {
  if (key >= 0 && key < 128)
    return key;
  if (key >= KEY_SPECIAL && key < KEY_SPECIAL + 16)
    return 128 + (key - KEY_SPECIAL);
  if (key >= KEY_META && key < KEY_META + 128)
    return 144 + key - KEY_META;
  return -1;
}

static void quit_now(editor *e) {
  e->running = 0;
}

static void cmd_quit(editor *e) {
  if (layout_has_unsaved(e))
    dispatch_confirm(e, quit_now, "Quit without saving?");
  else
    quit_now(e);
}

void dispatch_confirm(editor *e, command action, const char *prompt) {
  dispatch_confirm_with_cancel(e, action, NULL, prompt);
}

void dispatch_confirm_with_cancel(editor *e, command action, command cancel,
                                  const char *prompt) {
  e->confirmation = action;
  e->confirmation_cancel = cancel;
  e->confirmation_prompt = prompt;
  e->prefix_active = 0;
}

// Command - SAVE
static void cmd_save(editor *e) {
  if (!e->view->doc->filename)
    return; 

  // Serialize the focused document.
  size_t total = 0;
  for (block *k = e->view->doc->buf.head; k; k = k->next)
    for (int i = 0; i < k->count; i++)
      total += strlen(k->lines[i]) + 1;

  char *text = malloc(total + 1);
  if (!text)
    return;

  size_t pos = 0;
  for (block *k = e->view->doc->buf.head; k; k = k->next)
    for (int i = 0; i < k->count; i++) {
      size_t len = strlen(k->lines[i]);
      memcpy(text + pos, k->lines[i], len);
      pos += len;
      text[pos++] = '\n';
    }
  text[pos] = '\0';

  if (file_write(e->view->doc->filename, text) == 0)
    e->view->doc->dirty = 0;

  free(text);
}

// Remove the active selection, if there is one. Returns 1 if it removed
// something, so callers can tell an erasing keystroke is already done.
static int drop_selection(editor *e) {
  if (!e->view->sel_active)
    return 0;

  selection_delete(e);
  e->view->doc->dirty = 1;
  dispatch_change(e);
  return 1;
}

// Command - DELETE forward
static void cmd_delete(editor *e) {
  // Delete a selection
  if (drop_selection(e))
    return;

  char *line = buffer_line(&e->view->doc->buf, e->view->cy);
  int len = line ? (int)strlen(line) : 0;

  // Delete a single character
  // Remove the whole cluster under the cursor, however many bytes that is
  if (e->view->cx < len) {
    int end = grapheme_next(line, e->view->cx);
    for (int i = end - e->view->cx; i > 0; i--)
      buffer_delete_char(&e->view->doc->buf, e->view->cy, e->view->cx);
    e->view->doc->dirty = 1;
    dispatch_change(e);

  // At the line end, pull the next line up
  } else if (e->view->cy < e->view->doc->buf.nlines - 1) {
    if (buffer_join_line(&e->view->doc->buf, e->view->cy) == 0) {
      e->view->doc->dirty = 1;
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
  if (e->view->cx > 0) {
    char *line = buffer_line(&e->view->doc->buf, e->view->cy);
    int start = line ? grapheme_prev(line, e->view->cx) : e->view->cx - 1;
    for (int i = e->view->cx - start; i > 0; i--)
      buffer_delete_char(&e->view->doc->buf, e->view->cy, start);
    e->view->cx = start;
    e->view->doc->dirty = 1;
    cursor_mark_column(e);
    dispatch_change(e);

  // At column 0, merge the line with the previous one
  } else if (e->view->cy > 0) {
    char *prev = buffer_line(&e->view->doc->buf, e->view->cy - 1);
    int prevlen = prev ? (int)strlen(prev) : 0;
    if (buffer_join_line(&e->view->doc->buf, e->view->cy - 1) == 0) {
      e->view->cy--;
      e->view->cx = prevlen;
      e->view->doc->dirty = 1;
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

void dispatch_bind_prefix_when(int key, command cmd, const char *keys,
                               const char *label, command_condition when) {
  if (key_slot(key) < 0)
    return;
  int index = 0;
  while (index < nprefix && prefix_entries[index].key != key)
    index++;
  if (index == nprefix) {
    if (nprefix == KEY_SLOTS)
      return;
    nprefix++;
  }
  prefix_entries[index] = (command_binding){
    .key = key, .cmd = cmd, .keys = keys, .label = label, .when = when,
    .pair_key = KEY_NONE, .pair_primary = KEY_NONE
  };
}

// Pairing is presentation metadata; both keys keep their own command and condition.
void dispatch_pair_prefix(int first_key, int second_key, const char *label) {
  command_binding *first = NULL, *second = NULL;
  for (int i = 0; i < nprefix; i++) {
    if (prefix_entries[i].key == first_key)
      first = &prefix_entries[i];
    if (prefix_entries[i].key == second_key)
      second = &prefix_entries[i];
  }
  if (!first || !second || first == second || !label)
    return;
  first->pair_key = second_key;
  second->pair_key = first_key;
  first->pair_primary = second->pair_primary = first_key;
  first->pair_label = second->pair_label = label;
}

void dispatch_bind_prefix(int key, command cmd, const char *keys,
                          const char *label) {
  dispatch_bind_prefix_when(key, cmd, keys, label, NULL);
}

void dispatch_bind_prefix_global(int key, command cmd, const char *keys,
                                 const char *label) {
  dispatch_bind_prefix(key, cmd, keys, label);
  for (int i = 0; i < nprefix; i++)
    if (prefix_entries[i].key == key)
      prefix_entries[i].preserve_focus = 1;
}

static int binding_visible(const command_binding *binding, const editor *e) {
  return binding->cmd && (!binding->when || binding->when(e));
}

int dispatch_prefix_count(const editor *e) {
  int count = 0;
  for (int i = 0; i < nprefix; i++)
    count += binding_visible(&prefix_entries[i], e);
  return count;
}

const command_binding *dispatch_prefix_at(const editor *e, int index) {
  for (int i = 0; i < nprefix; i++)
    if (binding_visible(&prefix_entries[i], e) && index-- == 0)
      return &prefix_entries[i];
  return NULL;
}

const command_binding *dispatch_prefix_find(const editor *e, int key) {
  for (int i = 0; i < nprefix; i++)
    if (prefix_entries[i].key == key && binding_visible(&prefix_entries[i], e))
      return &prefix_entries[i];
  return NULL;
}

void dispatch_register(module *m) {
  if (nmods < MAX_MODULES)
    mods[nmods++] = m;
}

void dispatch_init(editor *e) {
  memset(cmds, 0, sizeof cmds);
  nprefix = 0;
  e->prefix_active = e->prefix_scroll = 0;
  e->confirmation = NULL;
  e->confirmation_cancel = NULL;
  e->confirmation_prompt = NULL;
  layout_init(e);
  file_manager_init(e);
  // Built-in key bindings.
  dispatch_bind(KEY_UP,    cursor_up);
  dispatch_bind(KEY_DOWN,  cursor_down);
  dispatch_bind(KEY_LEFT,  cursor_left);
  dispatch_bind(KEY_RIGHT, cursor_right);
  dispatch_bind(KEY_CTRL_LEFT,        cursor_word_left);
  dispatch_bind(KEY_CTRL_RIGHT,       cursor_word_right);
  dispatch_bind(KEY_DELETE,           cmd_delete);
  dispatch_bind(KEY_BACKSPACE,        cmd_backspace);
  dispatch_bind(KEY_PGUP,             cursor_page_up);
  dispatch_bind(KEY_PGDOWN,           cursor_page_down);
  dispatch_bind(KEY_HOME,             cursor_home);
  dispatch_bind(KEY_END,              cursor_end);

  // Emacs movement and editing, with terminal navigation keys as aliases.
  dispatch_bind(CTRL('p'), cursor_up);
  dispatch_bind(CTRL('n'), cursor_down);
  dispatch_bind(CTRL('b'), cursor_left);
  dispatch_bind(CTRL('f'), cursor_right);
  dispatch_bind(META('b'), cursor_word_left);
  dispatch_bind(META('f'), cursor_word_right);
  dispatch_bind(CTRL('a'), cursor_home);
  dispatch_bind(CTRL('e'), cursor_end);
  dispatch_bind(META('v'), cursor_page_up);
  dispatch_bind(CTRL('v'), cursor_page_down);
  dispatch_bind(META('<'), cursor_file_start);
  dispatch_bind(META('>'), cursor_file_end);
  dispatch_bind(KEY_CTRL_HOME, cursor_file_start);
  dispatch_bind(KEY_CTRL_END, cursor_file_end);
  dispatch_bind(CTRL('d'), cmd_delete);
  dispatch_bind(CTRL('h'), cmd_backspace);
  dispatch_bind(CTRL(' '), cursor_select_toggle);
  dispatch_bind(CTRL('g'), selection_clear);
  dispatch_bind('\x1b', selection_clear);

  dispatch_bind_prefix(CTRL('s'), cmd_save, "C-s", "Save");
  dispatch_bind_prefix(CTRL('c'), cmd_quit, "C-c", "Quit");
  dispatch_bind_prefix('l', cursor_screen_bottom, "l", "Last visible line");

  // Module init hooks.
  for (int i = 0; i < nmods; i++)
    if (mods[i]->init)
      mods[i]->init(e);
  layout_bindings();
  file_manager_bindings();
}

// Default text input
static void edit_key(editor *e, int key) {
  int text = (key >= 32 && key < KEY_SPECIAL && key != KEY_BACKSPACE);

  // Not an editing key
  if (key != '\r' && key != '\n' && !text)
    return;

  // Typing over a selection replaces it, and either way it ends selection
  // mode: the anchor left behind would silently grab whatever the cursor
  // walks over next.
  drop_selection(e);
  selection_clear(e);

  // Enter
  if (key == '\r' || key == '\n') {
    if (buffer_insert_newline(&e->view->doc->buf, e->view->cy, e->view->cx) == 0) {
      e->view->cy++;
      e->view->cx = 0;
      e->view->doc->dirty = 1;
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
    if (buffer_insert_char(&e->view->doc->buf, e->view->cy, e->view->cx + i, seq[i]) != 0) {
      // Take back the bytes that did go in: half a sequence is not a
      // character, and it would break every offset on the line.
      while (i-- > 0)
        buffer_delete_char(&e->view->doc->buf, e->view->cy, e->view->cx);
      return;
    }
  e->view->cx += n;
  e->view->doc->dirty = 1;
  cursor_mark_column(e);
  dispatch_change(e);
}

void dispatch_key(editor *e, int key) {
  if (key == KEY_NONE)
    return;

  // A confirmation consumes its answer before commands or text can see it.
  if (e->confirmation) {
    command action = e->confirmation;
    command cancel = e->confirmation_cancel;
    e->confirmation = NULL;
    e->confirmation_cancel = NULL;
    e->confirmation_prompt = NULL;
    if (key == 'y')
      action(e);
    else if (cancel)
      cancel(e);
    return;
  }

  int slot = key_slot(key);
  if (e->prefix_active) {
    // Repeating the prefix leaves the menu open without inserting text.
    if (key == CTRL('x'))
      return;
    if (key == KEY_UP || key == KEY_DOWN || key == KEY_PGUP || key == KEY_PGDOWN) {
      int step = key == KEY_UP ? -1 : key == KEY_DOWN ? 1 :
                 key == KEY_PGUP ? -command_menu_capacity(e) : command_menu_capacity(e);
      command_menu_scroll(e, step);
      return;
    }
    const command_binding *binding = dispatch_prefix_find(e, key);
    command cmd = binding ? binding->cmd : NULL;
    e->prefix_active = 0;
    if (cmd) {
      if (!binding->preserve_focus)
        e->files.focused = 0;
      cmd(e);
    } else if (key != CTRL('g') && key != '\x1b')
      e->prefix_active = 1; // unknown suffix: keep valid choices visible
    return;
  }

  if (e->files.focused && key != CTRL('x')) {
    file_manager_key(e, key);
    return;
  }

  // An active prompt owns its keys, including C-g for cancellation.
  for (int i = 0; i < nmods; i++)
    if (mods[i]->on_key && mods[i]->on_key(e, key)) {
      return;
    }

  if (key == CTRL('x')) {
    e->prefix_active = 1;
    e->prefix_scroll = 0;
    return;
  }

  command cmd = slot >= 0 ? cmds[slot] : NULL;
  if (cmd)
    cmd(e);
  else
    edit_key(e, key);
}

// Format a direct key from its registry slot using the same C-/M- notation
// as the prefix menu. Terminal navigation aliases are named as well.
static void binding_name(int slot, char *name, size_t size) {
  static const char *special_names[] = {
    "Left", "Right", "Up", "Down", "C-Left", "C-Right", "Del",
    "PgUp", "PgDn", "Home", "End", "C-Home", "C-End"
  };
  if (slot >= 144) {
    char base[16];
    binding_name(slot - 144, base, sizeof base);
    snprintf(name, size, "M-%s", base);
  } else if (slot >= 128) {
    int index = slot - 128;
    snprintf(name, size, "%s",
             index < (int)(sizeof special_names / sizeof special_names[0])
                 ? special_names[index] : "");
  } else if (slot == 0)
    snprintf(name, size, "C-SPC");
  else if (slot == 27)
    snprintf(name, size, "Esc");
  else if (slot < 32)
    snprintf(name, size, "C-%c", slot <= 26 ? 'a' + slot - 1
                                         : slot == 30 ? '^' : slot + '@');
  else if (slot == KEY_BACKSPACE)
    snprintf(name, size, "Backspace");
  else if (slot == ' ')
    snprintf(name, size, "SPC");
  else
    snprintf(name, size, "%c", slot);
}

void dispatch_direct_bindings(command cmd, char *keys, size_t size) {
  if (!size)
    return;
  keys[0] = '\0';
  size_t used = 0;
  // Emacs bindings first, followed by the terminal navigation aliases.
  for (int i = 0; i < KEY_SLOTS; i++) {
    int slot = i < 128 ? i : i < 256 ? i + 16 : i - 128;
    if (!cmd || cmds[slot] != cmd)
      continue;
    char name[32];
    binding_name(slot, name, sizeof name);
    if (!name[0])
      continue;
    size_t needed = strlen(name) + (used ? 2 : 0);
    if (needed >= size - used)
      break;
    int n = snprintf(keys + used, size - used, "%s%s", used ? ", " : "", name);
    used += n;
  }
}

void dispatch_draw(editor *e, struct abuf *ab) {
  for (int i = 0; i < nmods; i++)
    if (mods[i]->on_draw)
      mods[i]->on_draw(e, ab);
  if (e->prefix_active)
    command_menu_draw(e, ab);
}

void dispatch_change(editor *e) {
  layout_changed(e);
  for (int i = 0; i < nmods; i++)
    if (mods[i]->on_change)
      mods[i]->on_change(e);
}

void dispatch_shutdown(editor *e) {
  for (int i = 0; i < nmods; i++)
    if (mods[i]->shutdown)
      mods[i]->shutdown(e);
  file_manager_shutdown(e);
  layout_shutdown(e);
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
static int numbered_key(const char *par, int mod) {
  int ctrl = (mod == MOD_CTRL);
  switch (atoi(par)) {
    case 1:  return ctrl ? KEY_CTRL_HOME : KEY_HOME;
    case 3:  return KEY_DELETE;
    case 4:  return ctrl ? KEY_CTRL_END : KEY_END;
    case 5:  return KEY_PGUP;
    case 6:  return KEY_PGDOWN;
    case 7:  return ctrl ? KEY_CTRL_HOME : KEY_HOME; // some terminals send
    case 8:  return ctrl ? KEY_CTRL_END : KEY_END;   // 7/8 where others 1/4
    default: return 0;
  }
}

// Turn a completed escape sequence into a key. Returns 0 if it is a valid
// sequence that the editor simply has no key for.
static int escape_to_key(char final, const char *par) {
  int mod = escape_modifiers(par);

  switch (final) {
    case 'A':
      return KEY_UP;

    case 'B':
      return KEY_DOWN;

    case 'C':
      if (mod == MOD_CTRL)
        return KEY_CTRL_RIGHT;
      return KEY_RIGHT;

    case 'D':
      if (mod == MOD_CTRL)
        return KEY_CTRL_LEFT;
      return KEY_LEFT;

    case 'H':
      if (mod == MOD_CTRL)
        return KEY_CTRL_HOME;
      return KEY_HOME;

    case 'F':
      if (mod == MOD_CTRL)
        return KEY_CTRL_END;
      return KEY_END;

    case '~':
      return numbered_key(par, mod);

    default:
      return 0;
  }
}

// Consume a complete UTF-8 character, also for unsupported Meta characters.
static int read_character(unsigned char b) {
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

int read_key(void) {
  char c;
  if (READ(0, &c, 1) != 1)
    return KEY_NONE; // no input (timeout)

  if (c != '\x1b')
    return read_character((unsigned char)c);

  // The key is an escape sequence: ESC, then the byte that introduces it,
  // then the parameters, then the final byte.
  char introducer;
  if (READ(0, &introducer, 1) != 1)
    return '\x1b'; // ESC pressed on its own

  // CSI ("\x1b[") or SS3 ("\x1bO"), which some terminals use for the home key
  if (introducer != '[' && introducer != 'O') {
    int cp = read_character((unsigned char)introducer);
    return cp < 128 ? META(cp) : KEY_UNKNOWN;
  }

  char par[8];
  char final = read_escape_params(par, (int)sizeof par);
  if (final == 0)
    return '\x1b'; // incomplete sequence

  int key = escape_to_key(final, par);
  if (key == 0)
    return KEY_UNKNOWN; // input received, but no binding for this sequence

  return key;
}
