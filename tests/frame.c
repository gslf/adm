#include "frame.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static abuf shown;

// Frames are drawn like screen_refresh draws them: cursor hidden until the
// end, where a visible cursor is placed and shown again.
static abuf text(const char *s) {
  abuf ab = {0};
  ab_append(&ab, "\x1b[?2026h\x1b[?25l", 14);
  ab_append(&ab, s, (int)strlen(s));
  ab_append(&ab, "\x1b[?2026l", 8);
  return ab;
}

// Present a frame as screen_refresh does and return the bytes sent, or NULL
// when the frame had to be written as it is.
static char *present(const char *frame, int rows, int cols, int *result) {
  abuf next = text(frame), out = {0};
  *result = frame_update(&shown, &next, rows, cols, &out);
  if (*result < 0)
    frame_forget();
  else
    frame_shown();
  ab_free(&shown);
  shown = next;
  ab_append(&out, "", 1);
  return out.b;
}

static char *expect(const char *frame, int rows, int cols, int expected) {
  int result;
  char *out = present(frame, rows, cols, &result);
  assert(result == expected);
  if (result > 0) {
    assert(!strncmp(out, "\x1b[?2026h\x1b[?25l", 14));
    assert(strstr(out, "\x1b[?2026l") == out + strlen(out) - 8);
  }
  return out;
}

int main(void) {
  // The first frame paints every cell, blanks included.
  char *out = expect("\x1b[1;1Habc\x1b[2;1H\x1b[31mred", 3, 6, 1);
  assert(strstr(out, "abc   ") && strstr(out, "\x1b[0;31mred"));
  free(out);
  // Repeating the frame sends nothing; so does an equivalent one.
  free(expect("\x1b[1;1Habc\x1b[2;1H\x1b[31mred", 3, 6, 0));
  free(expect("\x1b[2;1H\x1b[0;31mred\x1b[m\x1b[1;1Habc", 3, 6, 0));

  // One changed cell is one cursor move and one character.
  out = expect("\x1b[1;1Habd\x1b[2;1H\x1b[31mred", 3, 6, 1);
  assert(!strcmp(out, "\x1b[?2026h\x1b[?25l\x1b[1;3H\x1b[0md\x1b[?2026l"));
  free(out);
  // A colour change rewrites the cells it touches with the new colour.
  out = expect("\x1b[1;1Habd\x1b[2;1H\x1b[1;32mred", 3, 6, 1);
  assert(strstr(out, "\x1b[2;1H\x1b[0;1;32mred") && !strstr(out, "abd"));
  free(out);
  // Cursor movements reach the terminal even without changed cells.
  out = expect("\x1b[1;1Habd\x1b[2;1H\x1b[1;32mred\x1b[3;4H\x1b[?25h", 3, 6, 1);
  assert(!strcmp(out, "\x1b[?2026h\x1b[?25l\x1b[3;4H\x1b[?25h\x1b[?2026l"));
  free(out);
  free(expect("\x1b[1;1Habd\x1b[2;1H\x1b[1;32mred\x1b[3;4H\x1b[?25h", 3, 6, 0));
  out = expect("\x1b[1;1Habd\x1b[2;1H\x1b[1;32mred\x1b[3;4H\x1b[?25l", 3, 6, 1);
  assert(!strcmp(out, "\x1b[?2026h\x1b[?25l\x1b[?2026l"));
  free(out);

  // Changes a few cells apart share one run instead of two cursor moves.
  free(expect("\x1b[1;1Hxbcdefghij", 1, 10, 1));
  out = expect("\x1b[1;1HXbcdEfghij", 1, 10, 1);
  assert(strstr(out, "\x1b[1;1H\x1b[0mXbcdE") && !strstr(out, "fghij"));
  free(out);
  out = expect("\x1b[1;1HXbcdEfghiJ", 1, 10, 1);
  assert(strstr(out, "\x1b[1;10H\x1b[0mJ"));
  free(out);

  // A wide cluster covers two cells; replacing it rewrites both halves.
  free(expect("\x1b[1;1Ha\xe7\x95\x8c" "b", 1, 6, 1));
  out = expect("\x1b[1;1Haxyb", 1, 6, 1);
  assert(strstr(out, "\x1b[1;2H\x1b[0mxy"));
  free(out);
  // Narrow cells turning into a wide one, then text after a wide cluster:
  // the cursor is placed again after it rather than trusting its width.
  out = expect("\x1b[1;1Ha\xe7\x95\x8c" "c", 1, 6, 1);
  assert(strstr(out, "\x1b[1;2H\x1b[0m\xe7\x95\x8c\x1b[1;4Hc"));
  free(out);
  // Overwriting half of a wide cluster erases the other half.
  out = expect("\x1b[1;1Ha\xe7\x95\x8c" "c\x1b[1;3HZ", 1, 6, 1);
  assert(strstr(out, "\x1b[1;2H\x1b[0m Z"));
  free(out);

  // Tabs move to the next stop without writing the cells they skip.
  free(expect("\x1b[1;1H..........", 1, 10, 1));
  out = expect("\x1b[1;1H..........\x1b[1;1Ha\tb", 1, 10, 1);
  assert(!strcmp(out, "\x1b[?2026h\x1b[?25l\x1b[1;1H\x1b[0ma\x1b[1;9Hb\x1b[?2026l"));
  free(out);

  // Anything else is written whole, and the next frame repaints all cells.
  free(expect("\x1b[1;1Hline\r\n", 2, 10, -1));
  free(expect("\x1b[2J\x1b[1;1Hx", 2, 10, -1));
  free(expect("\x1b[1;1H\x1b[6mblink", 2, 10, -1));
  free(expect("\x1b[1;1H\x1b[?1049h", 2, 10, -1));
  free(expect("\x1b[1;9Hlong", 2, 10, -1)); // would wrap
  out = expect("\x1b[1;1Hok", 2, 10, 1);
  assert(strstr(out, "ok        ") && strstr(out, "\x1b[2;1H"));
  free(out);
  // A new size repaints everything, as does forgetting the screen.
  out = expect("\x1b[1;1Hok", 2, 12, 1);
  assert(strstr(out, "ok          "));
  free(out);
  frame_forget();
  out = expect("\x1b[1;1Hok", 2, 12, 1);
  assert(strstr(out, "ok          "));
  free(out);
  // Direct and indexed colours survive the round trip.
  out = expect("\x1b[1;1H\x1b[38;2;1;2;3;48;5;200mok", 2, 12, 1);
  assert(strstr(out, "\x1b[0;38;2;1;2;3;48;5;200mok"));
  free(out);

  frame_forget();
  ab_free(&shown);
  return 0;
}
