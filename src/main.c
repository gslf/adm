#include <stdio.h>

#include "terminal.h"
#include "dispatch.h"
#include "screen.h"

int main(int argc, char *argv[]) {
  editor e;
  e.filename = (argc > 1) ? argv[1] : NULL;

  // Buffer loading
  if (load(&e.buf, e.filename) == -1) {
    fprintf(stderr, "BUFFER LOAD ERROR: %s\n", e.filename ? e.filename : "");
    return 1;
  }
  e.cx = e.cy = e.rowoff = e.coloff = 0;
  e.selx = e.sely = e.sel_active = e.sticky = 0;
  e.dirty = 0;
  e.running = 1;

  // Raw terminal init
  if (init_raw() == -1) {
    fprintf(stderr, "TERMINAL INITIALIZATION ERROR\n");
    buffer_free(&e.buf);
    return 1;
  }
  termsize ts = get_size();
  e.rows = ts.rows > 0 ? ts.rows : 24;
  e.cols = ts.cols > 0 ? ts.cols : 80;

  // Key bindings and modules dispatch
  dispatch_init(&e);

  // Main loop
  while (e.running) {
    if (term_resized()) {
      termsize s = get_size();
      if (s.rows > 0) {
        e.rows = s.rows;
        e.cols = s.cols;
      }
    }

    screen_refresh(&e);

    int k = read_key();
    if (k)
      dispatch_key(&e, k);
  }

  screen_clear();
  dispatch_shutdown(&e);
  buffer_free(&e.buf);
  restore();
  return 0;
}
