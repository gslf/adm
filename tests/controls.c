#include "clipboard.h"
#include "command_menu.h"
#include "cursor.h"
#include "screen.h"
#include "search.h"

#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>

static void test_reader(void) {
  int fds[2];
  int original = dup(0);
  assert(original >= 0 && pipe(fds) == 0);
  const char input[] = "\0\x1b" "b\x1b" "f\x1b<v\x1b[1;5C"
                       "\x1b[H\xc3\xa8\x1b\xc3\xa8x"
                       "\x1b\x1b[B\x1b\x18\x1b\x07x\x1b\x1bx\x1b[\x1b[A\x1b";
  assert(write(fds[1], input, sizeof input - 1) == sizeof input - 1);
  close(fds[1]);
  assert(dup2(fds[0], 0) == 0);
  close(fds[0]);
  assert(read_key() == CTRL(' '));
  assert(read_key() == META('b'));
  assert(read_key() == META('f'));
  assert(read_key() == META('<'));
  assert(read_key() == 'v');
  assert(read_key() == KEY_CTRL_RIGHT);
  assert(read_key() == KEY_HOME);
  assert(read_key() == 0xe8);
  assert(read_key() == KEY_UNKNOWN); // Unsupported Meta UTF-8 is fully consumed.
  assert(read_key() == 'x');
  // Esc and Ctrl keys right after Esc are never lost in a Meta combination.
  assert(read_key() == '\x1b');
  assert(read_key() == KEY_DOWN);
  assert(read_key() == '\x1b');
  assert(read_key() == CTRL('x'));
  assert(read_key() == '\x1b');
  assert(read_key() == CTRL('g'));
  assert(read_key() == 'x');
  assert(read_key() == '\x1b');
  assert(read_key() == META('x'));
  assert(read_key() == '\x1b'); // a sequence cut short by the next key
  assert(read_key() == KEY_UP);
  assert(read_key() == '\x1b');
  assert(read_key() == KEY_NONE);
  assert(dup2(original, 0) == 0);
  close(original);
}

static void test_escape_prefix(void) {
  int fds[2];
  int original = dup(0);
  assert(original >= 0 && pipe(fds) == 0);
  assert(dup2(fds[0], 0) == 0);
  close(fds[0]);
  // A lone Esc acts at once; x typed right after it still means M-x.
  assert(write(fds[1], "\x1b", 1) == 1);
  assert(read_key() == '\x1b');
  assert(write(fds[1], "xx", 2) == 2);
  assert(read_key() == META('x'));
  assert(read_key() == 'x');
  // A key without a Meta command, or one typed later, is the plain key.
  assert(write(fds[1], "\x1b", 1) == 1);
  assert(read_key() == '\x1b');
  assert(write(fds[1], "a", 1) == 1);
  assert(read_key() == 'a');
  assert(write(fds[1], "\x1b", 1) == 1);
  assert(read_key() == '\x1b');
  struct timespec later = {0, 150000000};
  nanosleep(&later, NULL);
  assert(write(fds[1], "x", 1) == 1);
  assert(read_key() == 'x');
  close(fds[1]);
  assert(read_key() == KEY_NONE);
  assert(dup2(original, 0) == 0);
  close(original);
}
#endif

static void type(editor *e, const char *s) {
  while (*s)
    dispatch_key(e, *s == '\n' ? (++s, '\r') : (unsigned char)*s++);
}

static void prefix(editor *e, int suffix) {
  dispatch_key(e, CTRL('x'));
  assert(e->prefix_active);
  dispatch_key(e, suffix);
  assert(!e->prefix_active);
}

// The drawn frame as plain text: escape sequences dropped, and a line break
// wherever the cursor moves.
static void frame_text(editor *e, char *text, size_t size) {
  abuf ab = {0};
  dispatch_draw(e, &ab);
  size_t n = 0;
  for (int i = 0; i < ab.len && n + 1 < size; i++) {
    if (ab.b[i] != '\x1b') {
      text[n++] = ab.b[i];
      continue;
    }
    while (i + 1 < ab.len && !isalpha((unsigned char)ab.b[i + 1]))
      i++;
    if (++i < ab.len && ab.b[i] == 'H')
      text[n++] = '\n';
  }
  text[n] = '\0';
  ab_free(&ab);
}

// Whether a line of `text` holds `first` with `second` further right.
static int on_one_line(const char *text, const char *first, const char *second) {
  for (const char *at = strstr(text, first); at; at = strstr(at + 1, first)) {
    const char *end = strchr(at, '\n');
    const char *found = strstr(at + strlen(first), second);
    if (found && (!end || found < end))
      return 1;
  }
  return 0;
}

static void frame_raw(editor *e, abuf *ab) {
  ab->len = 0;
  dispatch_draw(e, ab);
  ab_append(ab, "\0", 1);
}

static int only_in_split(const editor *e) {
  return e->windows.count > 1;
}

int main(int argc, char **argv) {
  assert(argc == 2);
#ifndef _WIN32
  test_reader();
  test_escape_prefix();
#endif
  editor e = {.document.filename = argv[1], .rows = 24, .cols = 80, .running = 1};
  buffer_init(&e.document.buf);
  dispatch_register(clipboard_module());
  dispatch_register(search_module());
  dispatch_init(&e);
  type(&e, "abc def\nghi\nabc");

  dispatch_key(&e, META('<'));
  assert(e.view->cx == 0 && e.view->cy == 0);
  dispatch_key(&e, CTRL('f'));
  assert(e.view->cx == 1 && !e.view->sel_mode);
  dispatch_key(&e, KEY_LEFT);
  assert(e.view->cx == 0 && !e.view->sel_mode);
  dispatch_key(&e, META('f'));
  assert(e.view->cx == 4); // Existing word motion goes to the next word's start.
  dispatch_key(&e, META('b'));
  assert(e.view->cx == 0);
  dispatch_key(&e, CTRL('e'));
  assert(e.view->cx == 7 && e.view->cy == 0);
  dispatch_key(&e, CTRL('a'));
  dispatch_key(&e, KEY_DOWN);
  assert(e.view->cx == 0 && e.view->cy == 1);
  dispatch_key(&e, KEY_UP);
  assert(e.view->cy == 0);
  dispatch_key(&e, CTRL('v'));
  assert(e.view->cy == 2);
  dispatch_key(&e, META('v'));
  assert(e.view->cy == 0);
  dispatch_key(&e, META('>'));
  assert(e.view->cy == 2 && e.view->cx == 3);

  dispatch_key(&e, META('<'));
  dispatch_key(&e, CTRL(' '));
  dispatch_key(&e, CTRL('f'));
  dispatch_key(&e, CTRL('x'));
  assert(e.prefix_active && e.view->sel_active);
  dispatch_key(&e, '!');
  assert(e.prefix_active && !strcmp(buffer_line(&e.document.buf, 0), "abc def"));
  dispatch_key(&e, CTRL('g'));
  assert(!e.prefix_active && e.view->sel_active); // Cancel only the modal.
  dispatch_key(&e, CTRL('g'));
  assert(!e.view->sel_mode && !e.view->sel_active);

  // C-x SPC enters the same selection mode as C-SPC, and leaves it again.
  prefix(&e, ' ');
  assert(e.view->sel_mode && !e.view->sel_active);
  dispatch_key(&e, CTRL('f'));
  assert(e.view->sel_active && selection_char_count(&e) == 1);
  prefix(&e, ' ');
  assert(!e.view->sel_mode && !e.view->sel_active);

#ifndef _WIN32
  // The runner removes clipboard helpers from PATH: use only the internal
  // clipboard and OSC 52, never the desktop clipboard during these tests.
  dispatch_key(&e, CTRL('a'));
  dispatch_key(&e, CTRL(' '));
  dispatch_key(&e, CTRL('f'));
  dispatch_key(&e, META('w'));
  assert(!e.view->sel_active && !strcmp(buffer_line(&e.document.buf, 0), "abc def"));
  dispatch_key(&e, CTRL('e'));
  dispatch_key(&e, CTRL('y'));
  assert(!strcmp(buffer_line(&e.document.buf, 0), "abc defa"));
  dispatch_key(&e, KEY_BACKSPACE);
  dispatch_key(&e, CTRL('a'));
  dispatch_key(&e, CTRL(' '));
  dispatch_key(&e, CTRL('f'));
  dispatch_key(&e, CTRL('w'));
  assert(!strcmp(buffer_line(&e.document.buf, 0), "bc def"));
  dispatch_key(&e, CTRL('y'));
  assert(!strcmp(buffer_line(&e.document.buf, 0), "abc def"));
#endif

  dispatch_key(&e, META('<'));
  dispatch_key(&e, CTRL('d'));
  assert(!strcmp(buffer_line(&e.document.buf, 0), "bc def"));
  dispatch_key(&e, 'a');
  dispatch_key(&e, META('<'));
  dispatch_key(&e, CTRL('s'));
  type(&e, "abc");
  unsigned long long matches; int done = 0;
  for (int attempt = 0; attempt < 10000 && !done; attempt++) {
    search_status(&e, &matches, &done);
#ifdef _WIN32
    Sleep(1);
#else
    usleep(1000);
#endif
  }
  assert(done && matches == 2);
  assert(e.view->sel_active && e.view->cy == 0);
  dispatch_key(&e, CTRL('s'));
  assert(e.view->cy == 2);
  dispatch_key(&e, CTRL('r'));
  assert(e.view->cy == 0);
  dispatch_key(&e, CTRL('g'));
  assert(!e.view->sel_active && e.view->cy == 0 && e.view->cx == 0);

  prefix(&e, 'l');
  type(&e, "2");
  assert(e.view->cy == 1);
  dispatch_key(&e, CTRL('g'));
  assert(e.view->cy == 0);
  prefix(&e, 'l');
  type(&e, "999");
  dispatch_key(&e, '\r');
  assert(e.view->cy == 2);
  dispatch_key(&e, META('<'));
  cursor_screen_bottom(&e);
  assert(e.view->cy == 2);

  // Commands sit under headings in a fixed order; on 80x24 two columns hold
  // them all, with nothing to scroll.
  char menu[32768];
  abuf ab = {0};
  dispatch_key(&e, CTRL('x'));
  frame_text(&e, menu, sizeof menu);
  assert(strstr(menu, "adm - Command Center"));
  assert(command_menu_count(&e) <= command_menu_capacity(&e));
  assert(on_one_line(menu, " FILE ", " SIDEBAR "));
  assert(strstr(menu, " FILE ") < strstr(menu, " EDIT ") &&
         strstr(menu, " EDIT ") < strstr(menu, " SEARCH "));
  assert(strstr(menu, " SPLITS ") && strstr(menu, " TABS ") && !strstr(menu, " MORE "));
  assert(strstr(menu, "C-s  Save") && strstr(menu, "]/[  Grow / shrink height"));
  // Direct shortcuts sit at the right of their row.
  assert(on_one_line(menu, "SPC  Selection mode", "C-SPC"));
  assert(on_one_line(menu, "s    Search file", "C-s"));
  assert(!on_one_line(menu, "C-s  Save", "C-s"));
  assert(on_one_line(menu, "?: help", "Esc: close") && !strstr(menu, "Up/Down: scroll"));
  assert(!strstr(menu, "Help")); // ? lives in the bottom bar, not in a group

  // A command that cannot run now keeps its place, dimmed, and its key says
  // why nothing happened instead of closing the menu.
  frame_raw(&e, &ab);
  assert(strstr(ab.b, "\x1b[2mc\x1b[22m") && strstr(ab.b, "\x1b[1mq\x1b[22m"));
  dispatch_key(&e, 'c');
  assert(e.prefix_active && !strcmp(e.notice, "Close split: not available here"));

  // Narrower terminals drop the shortcuts before a column; the narrowest
  // stack every group in one scrolling column.
  e.cols = 70;
  frame_text(&e, menu, sizeof menu);
  assert(on_one_line(menu, " FILE ", " SIDEBAR ") && !strstr(menu, "C-SPC"));
  e.cols = 40;
  frame_text(&e, menu, sizeof menu);
  assert(!on_one_line(menu, " FILE ", " SIDEBAR ") && strstr(menu, "C-SPC"));
  assert(strstr(menu, "Up/Down: scroll"));
  e.cols = 80;
  dispatch_key(&e, '\x1b');

  // Rows follow the live registry: a command no group lists lands under
  // MORE, and shortcuts track rebinding and removal.
  e.rows = 40;
  e.cols = 100;
  dispatch_bind(CTRL('p'), cursor_up);
  dispatch_bind(CTRL('n'), cursor_down);
  dispatch_bind_prefix('p', cursor_up, "p", "Move up");
  dispatch_bind_prefix('<', cursor_file_start, "<", "File start");
  dispatch_key(&e, CTRL('x'));
  frame_text(&e, menu, sizeof menu);
  assert(strstr(menu, " MORE ") && strstr(menu, "p    Move up"));
  assert(on_one_line(menu, "Move up", "C-p, Up"));
  assert(on_one_line(menu, "<    File start", "M-<, C-Home"));
  dispatch_bind(KEY_UP, NULL);
  frame_text(&e, menu, sizeof menu);
  assert(on_one_line(menu, "Move up", "C-p") && !strstr(menu, "C-p, Up"));
  dispatch_bind(CTRL('p'), cursor_down);
  frame_text(&e, menu, sizeof menu);
  assert(strstr(menu, "Move up") && !on_one_line(menu, "Move up", "C-p"));
  dispatch_bind(CTRL('p'), cursor_up);
  dispatch_bind(KEY_UP, cursor_up);
  dispatch_bind_prefix('o', cursor_down, "o", "Move down");
  dispatch_pair_prefix('p', 'o', "Move up / down");
  frame_text(&e, menu, sizeof menu);
  assert(strstr(menu, "p/o  Move up / down") && !strstr(menu, "o    Move down"));
  assert(on_one_line(menu, "Move up / down", "C-p, Up/C-n, Down"));

  e.rows = 10;
  command_menu_scroll(&e, 1000);
  assert(e.prefix_scroll == command_menu_count(&e) - command_menu_capacity(&e));
  e.rows = 40;
  command_menu_scroll(&e, 0);

  // Half a pair that cannot run is dimmed on its own; the row stays whole.
  dispatch_bind_prefix_when('p', cursor_up, "p", "Move up", only_in_split);
  dispatch_pair_prefix('p', 'o', "Move up / down");
  frame_raw(&e, &ab);
  assert(strstr(ab.b, "\x1b[2mp\x1b[22m") && strstr(ab.b, "\x1b[1mo\x1b[22m"));
  frame_text(&e, menu, sizeof menu);
  assert(strstr(menu, "p/o  Move up / down"));
  dispatch_key(&e, 'p');
  assert(e.prefix_active && !strcmp(e.notice, "Move up: not available here"));

  dispatch_bind_prefix('p', cursor_up, "p", "Move up");
  frame_text(&e, menu, sizeof menu); // Rebinding breaks the old pair.
  assert(strstr(menu, "p    Move up") && strstr(menu, "o    Move down"));
  assert(!strstr(menu, "Move up / down"));
  ab_free(&ab);
  e.rows = 24;
  e.cols = 80;
  dispatch_key(&e, 'p');
  assert(!e.prefix_active && e.view->cy == 1);

  const int cancel_keys[] = {'n', 'x', 'Y', KEY_DELETE, CTRL(' '), KEY_UNKNOWN};
  int cy = e.view->cy, cx = e.view->cx;
  for (size_t i = 0; i < sizeof cancel_keys / sizeof cancel_keys[0]; i++) {
    prefix(&e, CTRL('c'));
    assert(e.running && e.confirmation);
    dispatch_key(&e, KEY_NONE);
    assert(e.confirmation);
    dispatch_key(&e, cancel_keys[i]);
    assert(e.running && !e.confirmation && !e.prefix_active);
    assert(e.view->cy == cy && e.view->cx == cx);
    assert(!strcmp(buffer_line(&e.document.buf, 0), "abc def"));
  }
  prefix(&e, CTRL('c'));
  dispatch_key(&e, CTRL('x'));
  assert(!e.confirmation && e.prefix_active);
  dispatch_key(&e, CTRL('c'));
  assert(e.confirmation && !e.prefix_active);
  dispatch_key(&e, '\x1b');
  assert(e.running && !e.confirmation && !e.prefix_active);
  prefix(&e, CTRL('c'));
  dispatch_key(&e, 'y');
  assert(!e.running && !e.confirmation);
  e.running = 1;
  prefix(&e, CTRL('s'));
  assert(!e.document.dirty && !e.confirmation);
  FILE *saved = fopen(argv[1], "rb");
  assert(saved);
  char text[64] = {0};
  assert(fread(text, 1, sizeof text - 1, saved) == 16);
  fclose(saved);
  assert(!strcmp(text, "abc def\nghi\nabc\n"));
  prefix(&e, CTRL('c'));
  assert(!e.running && !e.confirmation);
  dispatch_shutdown(&e);
  assert(e.document.views == 0 && !e.document.buf.head && !e.view);
  return 0;
}
