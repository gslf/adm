#include "navigation.h"
#include "dispatch.h"
#include "screen.h"

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
  dispatch_bind_prefix_global('?', help, "?", "Navigation help");
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
  "",
  "C-x f      Open File Explorer with focus",
  "C-x g      Open Git Manager with focus",
  "C-x l      Go to Line",
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
  "C-z / M-z  Undo / redo (also C-x z / C-x Z)",
  "C-x C-s    Save; C-x C-c quits",
  "Quick tab/split creation also works in MOVE.",
  "",
  "Up/Down or PgUp/PgDown scroll; ESC closes this help"
};
#define HELP_COUNT ((int)(sizeof help_lines / sizeof help_lines[0]))

static int help_capacity(const editor *e) { return e->rows > 4 ? e->rows - 4 : 1; }
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

void navigation_draw(editor *e, abuf *ab) {
  if (!e->help_active || e->cols < 1 || e->rows < 3) return;
  help_clamp(e);
  int height = help_capacity(e);
  if (height > HELP_COUNT) height = HELP_COUNT;
  int width = e->cols < 80 ? e->cols : 80;
  int x = (e->cols - width) / 2, y = (e->rows - 2 - height) / 2 + 1;
  for (int i = 0; i < height; i++) {
    screen_position(ab, x, y + i);
    ab_append(ab, "\x1b[97;44m", 8);
    int used = screen_text(ab, help_lines[e->help_scroll + i], width);
    screen_repeat(ab, ' ', width - used);
    ab_append(ab, "\x1b[m", 3);
  }
}
