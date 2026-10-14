#include <stdio.h>
#include <stdlib.h>

#include "terminal.h"
#include "dispatch.h"
#include "screen.h"
#include "clipboard.h"
#include "search.h"
#include "path.h"
#include "lsp.h"

int main(int argc, char *argv[]) {
  editor e = {0};
  char *filename = NULL;
  if (argc > 1 && path_is_directory(argv[1])) {
    // A directory argument becomes the workspace root and opens no document.
    e.files.workspace_root = path_absolute(argv[1]);
    if (!e.files.workspace_root) {
      fprintf(stderr, "DIRECTORY OPEN ERROR: %s\n", argv[1]);
      return 1;
    }
  } else if (argc > 1) {
    e.document.filename = argv[1];
    filename = path_absolute(argv[1]);
    if (filename) {
      e.document.filename = filename;
      e.document.owns_filename = 1;
    }
  }

  // Buffer loading
  if (load(&e.document.buf, e.document.filename) == -1) {
    fprintf(stderr, "BUFFER LOAD ERROR: %s\n", e.document.filename ? e.document.filename : "");
    free(filename);
    return 1;
  }
  e.running = 1;

  // Raw terminal init
  if (init_raw() == -1) {
    fprintf(stderr, "TERMINAL INITIALIZATION ERROR\n");
    buffer_free(&e.document.buf);
    free(filename);
    free(e.files.workspace_root);
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
      screen_invalidate(); // the terminal may have reflowed or cleared it
      termsize s = get_size();
      if (s.rows > 0) {
        e.rows = s.rows;
        e.cols = s.cols;
      }
    }

    dispatch_tick(&e);
    screen_refresh(&e);

    int k = term_wait(lsp_busy(&e) ? 5 : 100) ? read_key() : KEY_NONE;
    if (k != KEY_NONE)
      dispatch_key(&e, k);
  }

  screen_clear();
  dispatch_shutdown(&e);
  restore();
  return 0;
}
