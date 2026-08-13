#ifndef SEARCH_H
#define SEARCH_H

#include "dispatch.h"

// Incremental search with regular expressions, and go to line.
//
// Register the module before dispatch_init() and it binds itself to Ctrl-F
// (search) and Ctrl-G (go to line). While the search prompt is open: type to
// search, Enter keeps the cursor on the match, Esc goes back to where the
// search started, Down (or Ctrl-F again) jumps to the next match and Up to
// the previous one. The GOTO prompt takes a line number and follows it live,
// with the same Enter and Esc.
module *search_module(void);

#endif
