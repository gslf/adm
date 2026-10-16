#include "theme.h"
#include "navigation.h"
#include "dispatch.h"
#include "screen.h"

#include <string.h>

static void left(editor *e) { layout_move(e, -1, 0); }
static void down(editor *e) { layout_move(e, 0, 1); }
static void up(editor *e) { layout_move(e, 0, -1); }
static void right(editor *e) { layout_move(e, 1, 0); }
static void next(editor *e) { tabs_focus(e, 1); e->sidebar.focused = 0; }
static void previous(editor *e) { tabs_focus(e, -1); e->sidebar.focused = 0; }
static void side(editor *e) {
  sidebar_show(e, e->sidebar.kind ? e->sidebar.kind :
                 e->sidebar.last ? e->sidebar.last : SIDEBAR_FILES);
}
static void move(editor *e) { e->move_active = 1; }
static void help(editor *e) { e->help_active = 1; e->help_scroll = 0; }

static command movement(int key) {
  switch (key) {
  case 'h': return left;
  case 'j': return down;
  case 'k': return up;
  case 'l': return right;
  case 'b': return side;
  case 'n': return next;
  case 'p': return previous;
  default: return NULL;
  }
}

int navigation_quick_key(int key) {
  return key > 0 && key <= 26 && movement(key + 'a' - 1) != NULL;
}

void navigation_bindings(void) {
  dispatch_bind_prefix_global('m', move, "m", "Move between splits");
  dispatch_bind_prefix_global('?', help, "?", "Help");
  const char *keys = "hjklbnp";
  for (; *keys; keys++)
    dispatch_bind(CTRL(*keys), movement(*keys));
}

static const char *help_lines[] = {
  "][ adm - Navigation help",
  "",
  "C-x m      Enter MOVE mode; ESC returns to editing",
  "h j k l    Move left / down / up / right between splits",
  "n / p      Next / previous tab (wrap at the ends)",
  "b          Focus the sidebar; l returns to the editor",
  "At a split edge, movement wraps to the opposite side.",
  "",
  "C-h C-j C-k C-l C-b C-n C-p",
  "           Same moves immediately, without entering MOVE",
  "Arrows move the text cursor or sidebar selection normally.",
  "Backspace deletes; Enter inserts a newline (C-j moves down).",
  "Text-entry prompts keep their own editing keys.",
  "Esc        Cancel current mode, menu or confirmation",
  "C-x / M-x  Open menus anywhere; cancel the current prompt",
  "C-x C-c    Quit from any mode (confirms unsaved edits)",
  "",
  "C-x f      Open File Explorer with focus",
  "C-x n      Rename selected Explorer file",
  "C-x d      Delete selected Explorer file (confirmation)",
  "C-x g      Open Git Manager with focus",
  "C-x l      Go to Line",
  "C-x w      Toggle word wrap (on by default)",
  "C-s / C-x s  Search current file; C-x S searches workspace",
  "C-x r / R  Search and replace in file / workspace",
  "Tab        Toggle TEXT / REGEX while entering a query",
  "Up / Down  Previous / next occurrence (C-r / C-s)",
  "Enter      Accept query; then enter replacement if needed",
  "y / n / a  Replace one / next / all after replacement entry",
  "Workspace replace-all confirms and writes saved files.",
  "C-t        New empty tab (also C-x t)",
  "C-q        Empty split above-below (also C-x q)",
  "C-u        Empty split side by side (also C-x u)",
  "C-x N      Create a named file in an empty split",
  "C-x } / {  Grow / shrink focused split or sidebar width",
  "C-x c      Close current split",
  "C-x k      Close current tab and all its splits",
  "Unsaved last references require confirmation before closing.",
  "C-SPC      Toggle selection mode (also C-x SPC)",
  "M-w / C-w / C-y  Copy / cut / paste the selection",
  "C-z / M-z  Undo / redo (also C-x z / C-x Z)",
  "C-x C-s    Save; C-x C-c quits",
  "Quick tab/split creation also works in MOVE.",
  "",
  "M-x        LSP menu (Alt-x or Esc then x)",
  "s/x/r      Start / stop / restart language server",
  "h/d/c      Symbol information / definition / completion",
  "n/p        Next / previous diagnostic",
  "N/f        Rename symbol / format current file",
  "a/t/?      Automatic startup / syntax colours / LSP help",
  "",
  "M-c        User Center (Alt-c or Esc then c)",
  "Up/Down    Choose a plugin command; Enter or its key runs it",
  "Esc / C-g  Close User Center; see HELP.md for configuration",
  "~/.adm.conf  Optional theme and C plugin configuration",
  "",
  "Up/Down or PgUp/PgDown scroll; ESC closes this help"
};
#define HELP_COUNT ((int)(sizeof help_lines / sizeof help_lines[0]))

// Blank space between the help text and the edges of its box.
#define HELP_PAD_X 3
#define HELP_PAD_Y 1

static int help_capacity(const editor *e) {
  int rows = e->rows - 4 - 2 * HELP_PAD_Y;
  return rows > 0 ? rows : 1;
}
static void help_clamp(editor *e) {
  int limit = HELP_COUNT - help_capacity(e);
  if (limit < 0) limit = 0;
  if (e->help_scroll < 0) e->help_scroll = 0;
  if (e->help_scroll > limit) e->help_scroll = limit;
}

int navigation_modal_key(editor *e, int key) {
  if (e->help_active) {
    if (key == '\x1b' || key == CTRL('g')) e->help_active = 0;
    else if (key == KEY_DOWN) e->help_scroll++;
    else if (key == KEY_UP) e->help_scroll--;
    else if (key == KEY_PGDOWN) e->help_scroll += help_capacity(e);
    else if (key == KEY_PGUP) e->help_scroll -= help_capacity(e);
    else if (key == KEY_HOME) e->help_scroll = 0;
    else if (key == KEY_END) e->help_scroll = HELP_COUNT;
    help_clamp(e);
    return 1;
  }
  if (!e->move_active) return 0;
  if (key == '\x1b') e->move_active = 0;
  else {
    if (navigation_quick_key(key)) key += 'a' - 1;
    command cmd = movement(key);
    if (cmd) cmd(e);
  }
  return 1;
}

// The box fits the widest line plus its padding, which shrinks to a single
// column on terminals too narrow for the whole of it.
void navigation_draw(editor *e, abuf *ab) {
  if (!e->help_active || e->cols < 1 || e->rows < 3) return;
  help_clamp(e);
  int lines = help_capacity(e);
  if (lines > HELP_COUNT) lines = HELP_COUNT;
  int text = 0;
  for (int i = 0; i < HELP_COUNT; i++)
    if ((int)strlen(help_lines[i]) > text) text = (int)strlen(help_lines[i]);
  int pad = (e->cols - text) / 2;
  if (pad > HELP_PAD_X) pad = HELP_PAD_X;
  if (pad < 1) pad = e->cols > 2 ? 1 : 0;
  int width = text + 2 * pad;
  if (width > e->cols) width = e->cols;
  int height = lines + 2 * HELP_PAD_Y;
  if (height > e->rows - 2) height = e->rows - 2;
  int x = (e->cols - width) / 2, y = (e->rows - 2 - height) / 2 + 1;
  for (int i = 0; i < height; i++) {
    screen_position(ab, x, y + i);
    theme_append(ab, THEME_ACTIVE);
    int line = i - HELP_PAD_Y, used = 0;
    if (line >= 0 && line < lines) {
      used = pad < width ? pad : width;
      screen_repeat(ab, ' ', used);
      used += screen_text(ab, help_lines[e->help_scroll + line], width - used);
    }
    screen_repeat(ab, ' ', width - used);
    theme_append(ab, THEME_NORMAL);
  }
}
