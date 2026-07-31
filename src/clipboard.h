#ifndef CLIPBOARD_H
#define CLIPBOARD_H

#include "dispatch.h"

// Copy, cut and paste of the selection through the system clipboard.
//
// Register the module before dispatch_init() and it binds itself to
// Ctrl-C (copy), Ctrl-X (cut) and Ctrl-V (paste).
module *clipboard_module(void);

#endif
