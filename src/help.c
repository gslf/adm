// The help page.
//
// Ctrl-/ covers the text area with the list of every key binding; any key
// puts the text back. The page is drawn over what screen_refresh already
// painted, one full row at a time, so nothing underneath shows through. It
// is a page to glance at, not to interact with, which is why the one thing
// a keystroke can do here is close it.
//
// Terminals have no separate code for Ctrl-/: they send 0x1F, the same byte
// as Ctrl-_, so both open it and the binding below is written CTRL('_').

#include "help.h"
#include "screen.h"

#include <stdio.h>
#include <string.h>

// All ASCII, so bytes are columns and clipping to the terminal width is
// plain truncation. Kept in step with the bindings in dispatch.c and the
// modules by hand, the same way the README is. At most 22 lines, so the
// whole page fits the text area of a classic 80x24 terminal.
static const char *page[] = {
  "][adm - key bindings",
  "",
  "Arrows            Move the cursor",
  "Ctrl-Left/Right   Previous / next word",
  "Home / End        Start / end of the line",
  "PgUp / PgDn       One screen up / down",
  "Ctrl-T            First line of the file    (also Ctrl-Home)",
  "Ctrl-E            Last line of the file     (also Ctrl-End)",
  "Ctrl-L            Last line on the screen",
  "Ctrl-G            Go to line",
  "",
  "Ctrl-B            Selection mode on / off   (Esc cancels)",
  "Ctrl-C / X / V    Copy / cut / paste",
  "",
  "Ctrl-F            Search (regex)",
  "  Up / Down       Previous / next match, while the search is open",
  "Ctrl-S            Save",
  "Ctrl-Q            Quit",
  "Ctrl-/            This page",
  "",
  "Press any key to go back",
};

#define PAGE_LINES ((int)(sizeof page / sizeof page[0]))
#define PAGE_MARGIN 2 // columns of air on the left

static int active;

// Command - open the help page
static void help_open(editor *e) {
  (void)e;
  active = 1;
}

static int help_on_key(editor *e, int key) {
  (void)e;
  (void)key;
  if (!active)
    return 0;

  // Whatever the key was, it closes the page and does nothing else: a stray
  // Ctrl-Q must not quit the editor from under a screen that hides it.
  active = 0;
  return 1;
}

static void help_on_draw(editor *e, abuf *ab) {
  if (!active)
    return;

  int th = e->rows - 2; // the text area between the two status bars
  if (th < 1)
    th = 1;

  char tmp[32];
  for (int y = 0; y < th; y++) {
    int n = snprintf(tmp, sizeof tmp, "\x1b[%d;1H\x1b[K", y + 2);
    ab_append(ab, tmp, n);

    if (y >= PAGE_LINES)
      continue; // rows past the page stay cleared

    int avail = e->cols - PAGE_MARGIN;
    if (avail <= 0)
      continue;

    for (int i = 0; i < PAGE_MARGIN; i++)
      ab_append(ab, " ", 1);

    if (y == 0)
      ab_append(ab, "\x1b[1m", 4); // the title, in bold

    const char *line = page[y];
    int len = (int)strlen(line);
    ab_append(ab, line, len < avail ? len : avail);

    if (y == 0)
      ab_append(ab, "\x1b[22m", 5);
  }
}

///////////////////////////////////
// MODULE

static void help_init(editor *e) {
  (void)e;
  dispatch_bind(CTRL('_'), help_open);
}

static module help = {
  .name = "help",
  .init = help_init,
  .on_key = help_on_key,
  .on_draw = help_on_draw,
  .on_change = NULL,
  .shutdown = NULL
};

module *help_module(void) {
  return &help;
}
