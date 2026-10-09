#include "screen.h"
#include "theme.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define dup _dup
#define dup2 _dup2
#define close _close
#define fileno _fileno
#else
#include <unistd.h>
#endif

static char *output_since(FILE *output, long *offset) {
  assert(!fflush(stdout));
  assert(!fseek(output, 0, SEEK_END));
  long end = ftell(output);
  assert(end >= *offset);
  size_t length = (size_t)(end - *offset);
  char *text = malloc(length + 1);
  assert(text);
  assert(!fseek(output, *offset, SEEK_SET));
  assert(fread(text, 1, length, output) == length);
  text[length] = 0;
  assert(!fseek(output, 0, SEEK_END));
  *offset = end;
  return text;
}

static void frame(editor *e, FILE *output, long *offset) {
  screen_refresh(e);
  char *text = output_since(output, offset);
  const char *begin = "\x1b[?2026h\x1b[?25l";
  const char *end = "\x1b[?2026l";
  size_t length = strlen(text);
  assert(length >= strlen(begin) + strlen(end));
  assert(!strncmp(text, begin, strlen(begin)));
  assert(!strcmp(text + length - strlen(end), end));
  free(text);
}

static void idle(editor *e, FILE *output, long *offset) {
  for (int i = 0; i < 10; i++) {
    dispatch_tick(e);
    screen_refresh(e);
  }
  char *text = output_since(output, offset);
  assert(!*text); // Idle polling must not blank/repaint the console.
  free(text);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  editor e = {.document.filename = argv[1], .rows = 24, .cols = 80, .running = 1};
  buffer_init(&e.document.buf);
  dispatch_init(&e);
  FILE *output = tmpfile();
  assert(output);
  assert(!fflush(stdout));
  int saved_stdout = dup(1);
  assert(saved_stdout >= 0 && dup2(fileno(output), 1) >= 0);
  long offset = 0;

  frame(&e, output, &offset);
  idle(&e, output, &offset);
  dispatch_key(&e, 'a');
  frame(&e, output, &offset);
  idle(&e, output, &offset);
  dispatch_key(&e, KEY_LEFT); // Cursor-only changes still reach the terminal.
  frame(&e, output, &offset);
  idle(&e, output, &offset);

  e.rows = 20;
  e.cols = 60;
  frame(&e, output, &offset);
  idle(&e, output, &offset);
  theme_palette palette;
  theme_defaults(&palette);
  assert(theme_set(&palette, "normal", "15,4") >= 0);
  theme_use(&palette);
  frame(&e, output, &offset);
  idle(&e, output, &offset);

  dispatch_key(&e, CTRL('x'));
  frame(&e, output, &offset);
  idle(&e, output, &offset);
  dispatch_key(&e, CTRL('g'));
  frame(&e, output, &offset);

  screen_clear();
  char *cleanup = output_since(output, &offset);
  assert(strstr(cleanup, "\x1b[?2026l") && strstr(cleanup, "\x1b[?25h"));
  free(cleanup);
  frame(&e, output, &offset); // Clearing invalidates the frame cache.
  idle(&e, output, &offset);
  screen_clear();
  assert(dup2(saved_stdout, 1) >= 0);
  close(saved_stdout);
  fclose(output);
  dispatch_shutdown(&e);
  theme_use(NULL);
  return 0;
}
