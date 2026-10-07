#include "clipboard.h"
#include "command_menu.h"
#include "cursor.h"
#include "screen.h"
#include "search.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>

static void test_reader(void) {
  int fds[2];
  int original = dup(0);
  assert(original >= 0 && pipe(fds) == 0);
  const char input[] = "\0\x1b" "b\x1b" "f\x1b<v\x1b[1;5C"
                       "\x1b[H\xc3\xa8\x1b\xc3\xa8x\x1b";
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
  assert(read_key() == '\x1b');
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

static int only_in_split(const editor *e) {
  return e->windows.count > 1;
}

int main(int argc, char **argv) {
  assert(argc == 2);
#ifndef _WIN32
  test_reader();
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
#ifndef _WIN32
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

  dispatch_key(&e, CTRL('x'));
  abuf ab = {0};
  dispatch_draw(&e, &ab);
  ab_append(&ab, "\0", 1);
  assert(strstr(ab.b, "adm - Command Center"));
  assert(strstr(ab.b, "Save") && strstr(ab.b, "Quit"));
  assert(strstr(ab.b, "Go to Line") && strstr(ab.b, "Move between splits"));
  assert(strstr(ab.b, " - Save") && !strstr(ab.b, "C-s [")); // No brackets without a direct binding.
  ab_free(&ab);
  dispatch_key(&e, '\x1b');

  // Display aliases from the live registry, including rebinding/removal.
  dispatch_bind(CTRL('p'), cursor_up);
  dispatch_bind(CTRL('n'), cursor_down);
  dispatch_bind_prefix('p', cursor_up, "p", "Move up");
  dispatch_bind_prefix('<', cursor_file_start, "<", "File start");
  dispatch_key(&e, CTRL('x'));
  dispatch_draw(&e, &ab);
  ab_append(&ab, "\0", 1);
  assert(strstr(ab.b, "p [C-p, Up]") && strstr(ab.b, " - Move up"));
  assert(strstr(ab.b, "< [M-<, C-Home] - File start"));
  ab_free(&ab);
  dispatch_bind(KEY_UP, NULL);
  dispatch_draw(&e, &ab);
  ab_append(&ab, "\0", 1);
  assert(strstr(ab.b, "p [C-p]") && strstr(ab.b, " - Move up"));
  assert(!strstr(ab.b, "p [C-p, Up]"));
  ab_free(&ab);
  dispatch_bind(CTRL('p'), cursor_down);
  dispatch_draw(&e, &ab);
  ab_append(&ab, "\0", 1);
  assert(strstr(ab.b, " - Move up"));
  assert(!strstr(ab.b, "p [C-p]"));
  ab_free(&ab);
  dispatch_bind(CTRL('p'), cursor_up);
  dispatch_bind(KEY_UP, cursor_up);
  dispatch_bind_prefix('n', cursor_down, "n", "Move down");
  dispatch_pair_prefix('p', 'n', "Move up / down");
  assert(command_menu_count(&e) == dispatch_prefix_count(&e));
  dispatch_draw(&e, &ab);
  ab_append(&ab, "\0", 1);
  assert(strstr(ab.b, "p [C-p, Up] / n [C-n, Down]"));
  assert(strstr(ab.b, " - Move up / down"));
  ab_free(&ab);

  e.rows = 10;
  command_menu_scroll(&e, 1000);
  assert(e.prefix_scroll == command_menu_count(&e) - command_menu_capacity(&e));
  e.rows = 24;
  command_menu_scroll(&e, 0);

  dispatch_bind_prefix_when('p', cursor_up, "p", "Move up", only_in_split);
  dispatch_pair_prefix('p', 'n', "Move up / down");
  assert(command_menu_count(&e) == dispatch_prefix_count(&e) + 1);
  dispatch_draw(&e, &ab);
  ab_append(&ab, "\0", 1);
  assert(strstr(ab.b, "n [C-n, Down]") && strstr(ab.b, " - Move down"));
  assert(!strstr(ab.b, "Move up / down"));
  ab_free(&ab);

  dispatch_bind_prefix('p', cursor_up, "p", "Move up");
  assert(command_menu_count(&e) == dispatch_prefix_count(&e) + 1); // Rebinding breaks the old pair.
  dispatch_key(&e, 'p');
  assert(!e.prefix_active && e.view->cy == 1);

  const int cancel_keys[] = {'n', 'x', 'Y', CTRL('x'), KEY_DELETE, CTRL(' '), KEY_UNKNOWN};
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
  dispatch_key(&e, CTRL('c'));
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
