#include <stdio.h>

#include "terminal.h"
#include "dispatch.h"
#include "screen.h"
#include "clipboard.h"
#include "search.h"

int main(int argc, char *argv[]) {
  editor e = {0};
  e.document.filename = (argc > 1) ? argv[1] : NULL;

  // Buffer loading
  if (load(&e.document.buf, e.document.filename) == -1) {
    fprintf(stderr, "BUFFER LOAD ERROR: %s\n", e.document.filename ? e.document.filename : "");
    return 1;
  }
  e.running = 1;

  // Raw terminal init
  if (init_raw() == -1) {
    fprintf(stderr, "TERMINAL INITIALIZATION ERROR\n");
    buffer_free(&e.document.buf);
    return 1;
  }
  termsize ts = get_size();
  e.rows = ts.rows > 0 ? ts.rows : 24;
  e.cols = ts.cols > 0 ? ts.cols : 80;

  // Key bindings and modules dispatch
  dispatch_register(clipboard_module());
  dispatch_register(search_module());
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

    dispatch_tick(&e);
    screen_refresh(&e);

    int k = read_key();
    if (k != KEY_NONE)
      dispatch_key(&e, k);
  }

  screen_clear();
  dispatch_shutdown(&e);
  restore();
  return 0;
}
