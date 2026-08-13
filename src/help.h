#ifndef HELP_H
#define HELP_H

#include "dispatch.h"

// The help page: every key binding on one screen.
//
// Register the module before dispatch_init() and it binds itself to Ctrl-/
// (which the terminal reports as Ctrl-_, so that combination works too).
// The page covers the text area until any key is pressed.
module *help_module(void);

#endif
