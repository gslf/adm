// Copy, cut and paste through the system clipboard.
//
// Reaching the clipboard is not the same everywhere, so this module tries
// three routes and stops at the first one that works:
//
//   1. the native clipboard  - Windows API, or one of the wl-copy / xclip /
//                              xsel / pbcopy helpers on POSIX desktops;
//   2. the terminal itself   - the OSC 52 escape sequence, the only route
//                              that survives an ssh session;
//   3. an internal register  - a private copy kept by the editor, so the
//                              keys always do something even on a bare tty.
//
// Everything below the PLATFORM section talks to the outside world through
// four functions only, which both platforms provide:
//
//   clipboard_platform_init()   prepare the platform, once at start-up
//   system_clipboard_copy()     push text out, 1 on success and 0 on failure
//   system_clipboard_paste()    pull text in, a fresh string or NULL
//   terminal_clipboard_copy()   route 2, used only when route 1 fails

#include "clipboard.h"
#include "cursor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Route 3: the editor's own copy of the last cut or copy.
static char *internal_clipboard = NULL;

///////////////////////////////////
// TEXT BUILDER
//
// A string that grows as text is appended to it, so that no caller has to
// guess a final size in advance. An allocation failure is remembered in
// `failed` instead of being reported on every single append: the caller
// checks once, at the end.

#define BUILDER_INITIAL_CAPACITY 8192

typedef struct {
  char *data;
  size_t length;
  size_t capacity;
  int failed;
} text_builder;

static void builder_init(text_builder *builder) {
  builder->data = NULL;
  builder->length = 0;
  builder->capacity = 0;
  builder->failed = 0;
}

static void builder_free(text_builder *builder) {
  free(builder->data);
  builder_init(builder);
}

// Make sure `extra` more bytes fit. Returns 0 if there is no memory left.
static int builder_reserve(text_builder *builder, size_t extra) {
  size_t needed = builder->length + extra;
  if (needed <= builder->capacity)
    return 1;

  size_t new_capacity = builder->capacity;
  if (new_capacity == 0)
    new_capacity = BUILDER_INITIAL_CAPACITY;
  while (new_capacity < needed)
    new_capacity *= 2;

  char *bigger = realloc(builder->data, new_capacity);
  if (bigger == NULL)
    return 0;

  builder->data = bigger;
  builder->capacity = new_capacity;
  return 1;
}

static void builder_append(text_builder *builder, const char *bytes,
                           size_t count) {
  if (builder->failed)
    return;

  if (!builder_reserve(builder, count)) {
    builder->failed = 1;
    return;
  }

  memcpy(builder->data + builder->length, bytes, count);
  builder->length += count;
}

static void builder_append_char(text_builder *builder, char c) {
  builder_append(builder, &c, 1);
}

// Terminate the text and hand the buffer over to the caller, who has to free
// it. Returns NULL if any append ran out of memory along the way. The builder
// is left empty either way.
static char *builder_finish(text_builder *builder) {
  if (builder->failed || !builder_reserve(builder, 1)) {
    builder_free(builder);
    return NULL;
  }

  builder->data[builder->length] = '\0';

  char *text = builder->data;
  builder_init(builder); // the buffer belongs to the caller now
  return text;
}

///////////////////////////////////
// PLATFORM: WINDOWS
#ifdef _WIN32

#include <windows.h>

static void clipboard_platform_init(void) {
  // Nothing to prepare: the clipboard is part of the operating system.
}

// Turn a UTF-8 string into the UTF-16 global block the clipboard expects.
// Returns NULL on failure.
static HGLOBAL utf8_to_global_utf16(const char *text) {
  // Called with a NULL destination the function does not convert, it only
  // measures. The -1 says "the string is NUL terminated", so the count that
  // comes back already includes the terminator.
  int wide_length = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
  if (wide_length <= 0)
    return NULL;

  // GMEM_MOVEABLE is mandatory: the clipboard takes the block over and moves
  // it around, so an ordinary malloc would not do.
  HGLOBAL block =
      GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)wide_length * sizeof(WCHAR));
  if (block == NULL)
    return NULL;

  // A movable block has to be locked before it has a usable address.
  WCHAR *wide_text = GlobalLock(block);
  if (wide_text == NULL) {
    GlobalFree(block);
    return NULL;
  }

  MultiByteToWideChar(CP_UTF8, 0, text, -1, wide_text, wide_length);
  GlobalUnlock(block);
  return block;
}

static int system_clipboard_copy(const char *text) {
  HGLOBAL block = utf8_to_global_utf16(text);
  if (block == NULL)
    return 0;

  if (!OpenClipboard(NULL)) {
    GlobalFree(block);
    return 0;
  }

  EmptyClipboard(); // one cannot become the owner without clearing it first

  if (!SetClipboardData(CF_UNICODETEXT, block)) {
    CloseClipboard();
    GlobalFree(block);
    return 0;
  }

  // The block belongs to the clipboard from here on: freeing it now would be
  // a double free.
  CloseClipboard();
  return 1;
}

// Turn clipboard UTF-16 into a fresh UTF-8 string, or NULL on failure.
static char *global_utf16_to_utf8(const WCHAR *wide_text) {
  int length = WideCharToMultiByte(CP_UTF8, 0, wide_text, -1, NULL, 0, NULL,
                                   NULL); // measure first, as above
  if (length <= 0)
    return NULL;

  char *text = malloc((size_t)length);
  if (text == NULL)
    return NULL;

  WideCharToMultiByte(CP_UTF8, 0, wide_text, -1, text, length, NULL, NULL);
  return text;
}

static char *system_clipboard_paste(void) {
  if (!IsClipboardFormatAvailable(CF_UNICODETEXT))
    return NULL;
  if (!OpenClipboard(NULL))
    return NULL;

  char *text = NULL;

  // This handle belongs to the clipboard: read it, never free it.
  HANDLE handle = GetClipboardData(CF_UNICODETEXT);
  if (handle != NULL) {
    WCHAR *wide_text = GlobalLock(handle);
    if (wide_text != NULL) {
      text = global_utf16_to_utf8(wide_text);
      GlobalUnlock(handle);
    }
  }

  CloseClipboard();
  return text;
}

static void terminal_clipboard_copy(const char *text) {
  // The native clipboard is always there, so route 2 is never needed.
  (void)text;
}

///////////////////////////////////
// PLATFORM: POSIX
#else

#include <signal.h>
#include <unistd.h>

// The helpers that own the clipboard on the common desktops, in the order we
// try them. Copy and paste commands sit side by side so that the two can
// never drift apart. Errors go to /dev/null on purpose: a missing tool must
// not scribble its complaint over the editor's screen.
typedef struct {
  const char *name;
  const char *copy_command;
  const char *paste_command;
} clipboard_helper;

static const clipboard_helper helpers[] = {
  {"wl-copy", "wl-copy 2>/dev/null", "wl-paste --no-newline 2>/dev/null"},
  {"xclip", "xclip -selection clipboard 2>/dev/null",
   "xclip -selection clipboard -o 2>/dev/null"},
  {"xsel", "xsel --clipboard --input 2>/dev/null",
   "xsel --clipboard --output 2>/dev/null"},
  {"pbcopy", "pbcopy 2>/dev/null", "pbpaste 2>/dev/null"}
};

static const int helper_count = (int)(sizeof helpers / sizeof helpers[0]);

// Which helper to use. Probing means starting up to four processes, so the
// answer is worked out once and then reused.
typedef enum {
  HELPERS_NOT_PROBED_YET,
  HELPER_FOUND,
  NO_HELPER_AVAILABLE
} helper_status;

static helper_status status = HELPERS_NOT_PROBED_YET;
static int chosen_helper = 0; // only meaningful once status is HELPER_FOUND

static void clipboard_platform_init(void) {
  // Writing to a helper that failed to start raises SIGPIPE, which by default
  // kills the editor. Ignoring it turns the same situation into a harmless
  // short write, which the code below already checks for.
  signal(SIGPIPE, SIG_IGN);
}

// Feed the text to one helper. Returns 1 only if every byte went through and
// the helper exited cleanly, which is also how we tell it exists at all.
static int copy_with_helper(const clipboard_helper *helper, const char *text) {
  FILE *pipe = popen(helper->copy_command, "w");
  if (pipe == NULL)
    return 0;

  size_t length = strlen(text);
  size_t written = fwrite(text, 1, length, pipe);
  int exit_status = pclose(pipe);

  return exit_status == 0 && written == length;
}

static int system_clipboard_copy(const char *text) {
  if (status == HELPER_FOUND)
    return copy_with_helper(&helpers[chosen_helper], text);
  if (status == NO_HELPER_AVAILABLE)
    return 0;

  for (int i = 0; i < helper_count; i++) {
    if (copy_with_helper(&helpers[i], text)) {
      status = HELPER_FOUND;
      chosen_helper = i;
      return 1;
    }
  }

  status = NO_HELPER_AVAILABLE;
  return 0;
}

#define HELPER_READ_CHUNK 4096

// Read everything one helper prints. Returns a fresh string, or NULL if the
// helper is missing or failed.
static char *paste_with_helper(const clipboard_helper *helper) {
  FILE *pipe = popen(helper->paste_command, "r");
  if (pipe == NULL)
    return NULL;

  text_builder output;
  builder_init(&output);

  for (;;) {
    char chunk[HELPER_READ_CHUNK];
    size_t read_count = fread(chunk, 1, sizeof chunk, pipe);
    builder_append(&output, chunk, read_count);

    if (read_count < sizeof chunk)
      break; // a short read means the helper is done
  }

  // A non-zero exit status means the helper is not the right one for this
  // desktop, so whatever came out of it cannot be trusted.
  if (pclose(pipe) != 0) {
    builder_free(&output);
    return NULL;
  }

  return builder_finish(&output);
}

static char *system_clipboard_paste(void) {
  if (status == HELPER_FOUND)
    return paste_with_helper(&helpers[chosen_helper]);
  if (status == NO_HELPER_AVAILABLE)
    return NULL;

  for (int i = 0; i < helper_count; i++) {
    char *text = paste_with_helper(&helpers[i]);
    if (text != NULL) {
      status = HELPER_FOUND;
      chosen_helper = i;
      return text;
    }
  }

  status = NO_HELPER_AVAILABLE;
  return NULL;
}

///////////////////////////////////
// OSC 52
//
// With no helper around, the text can still be handed to the terminal itself,
// Base64 encoded inside an escape sequence. This is the only route that works
// over ssh, where the real clipboard lives on the far end of the connection.

#define OSC52_PREFIX "\x1b]52;c;"
#define OSC52_TERMINATOR "\x07"

// Terminals cap how long a sequence they will accept, so past this size the
// attempt is not even worth making.
#define OSC52_MAX_INPUT 65536

static const char base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Encode up to three input bytes into exactly four Base64 characters. When
// fewer than three bytes are left the spare characters become padding.
static void encode_three_bytes(const char *input, size_t available,
                               char output[4]) {
  // The casts to unsigned char matter: without them a byte of 0x80 or above,
  // which any non-ASCII character in UTF-8 contains, would be sign extended
  // and corrupt the group.
  unsigned long first = (unsigned char)input[0];
  unsigned long second = (available > 1) ? (unsigned char)input[1] : 0;
  unsigned long third = (available > 2) ? (unsigned char)input[2] : 0;

  // Three bytes packed together, then read back out as four groups of 6 bits.
  unsigned long group = (first << 16) | (second << 8) | third;

  output[0] = base64_alphabet[(group >> 18) & 0x3f];
  output[1] = base64_alphabet[(group >> 12) & 0x3f];
  output[2] = (available > 1) ? base64_alphabet[(group >> 6) & 0x3f] : '=';
  output[3] = (available > 2) ? base64_alphabet[group & 0x3f] : '=';
}

static void terminal_clipboard_copy(const char *text) {
  size_t length = strlen(text);
  if (length == 0 || length > OSC52_MAX_INPUT)
    return;

  text_builder sequence;
  builder_init(&sequence);
  builder_append(&sequence, OSC52_PREFIX, sizeof OSC52_PREFIX - 1);

  for (size_t i = 0; i < length; i += 3) {
    char encoded[4];
    encode_three_bytes(text + i, length - i, encoded);
    builder_append(&sequence, encoded, sizeof encoded);
  }

  builder_append(&sequence, OSC52_TERMINATOR, sizeof OSC52_TERMINATOR - 1);

  // Straight to the terminal, not through printf: this is a control sequence
  // meant for the terminal itself and it must not sit in a stdio buffer.
  if (!sequence.failed)
    write(STDOUT_FILENO, sequence.data, sequence.length);

  builder_free(&sequence);
}

#endif

///////////////////////////////////
// SELECTION TEXT

// The selection with both ends sorted in document order. The end column is
// exclusive, as selection_range() returns it.
typedef struct {
  int first_row, first_col;
  int last_row, last_col;
} selection;

// Half open byte range [start, end) of a single line.
typedef struct {
  int start;
  int end;
} byte_range;

// Which part of `line`, the text of row `row`, falls inside the selection.
static byte_range selected_bytes_in_row(const selection *sel, int row,
                                        const char *line) {
  int line_length = (line != NULL) ? (int)strlen(line) : 0;

  byte_range range;
  // The first row starts where the selection does, the others at column 0;
  // the last row ends where the selection does, the others at end of line.
  range.start = (row == sel->first_row) ? sel->first_col : 0;
  range.end = (row == sel->last_row) ? sel->last_col : line_length;

  // A stored column can sit past the end of a shorter line, and on a single
  // row selection the two can even cross. Clamp so the range stays valid.
  if (range.end > line_length)
    range.end = line_length;
  if (range.start > range.end)
    range.start = range.end;

  return range;
}

// The selected text as a fresh string, or NULL if nothing is selected.
static char *selection_text(const editor *e) {
  selection sel;
  if (!selection_range(e, &sel.first_row, &sel.first_col, &sel.last_row,
                       &sel.last_col))
    return NULL;

  text_builder text;
  builder_init(&text);

  for (int row = sel.first_row; row <= sel.last_row; row++) {
    const char *line = buffer_line(&e->buf, row);
    byte_range range = selected_bytes_in_row(&sel, row, line);

    if (line != NULL)
      builder_append(&text, line + range.start,
                     (size_t)(range.end - range.start));

    // Rows are joined by a newline, exactly as the file stores them, which
    // means the last row of the selection does not get one.
    if (row < sel.last_row)
      builder_append_char(&text, '\n');
  }

  return builder_finish(&text);
}

// Take ownership of `text` and make it the current clipboard contents.
static void clipboard_set(char *text) {
  free(internal_clipboard);
  internal_clipboard = text;

  if (!system_clipboard_copy(text))
    terminal_clipboard_copy(text);
}

// Insert text at the cursor, one character at a time. Every flavour of line
// ending counts as a single break: "\r\n", a lone "\n" and a lone "\r".
static void insert_text_at_cursor(editor *e, const char *text) {
  for (int i = 0; text[i] != '\0'; i++) {
    char c = text[i];

    if (c == '\r' && text[i + 1] == '\n')
      continue; // skip it, the '\n' right after makes the break

    if (c == '\n' || c == '\r') {
      // Running out of memory stops the paste but leaves the buffer sound,
      // with everything inserted so far still in place.
      if (buffer_insert_newline(&e->buf, e->cy, e->cx) != 0)
        return;

      e->cy++;
      e->cx = 0;
      continue;
    }

    if (buffer_insert_char(&e->buf, e->cy, e->cx, c) != 0)
      return;

    e->cx++;
  }
}

///////////////////////////////////
// COMMANDS

static void clip_copy(editor *e) {
  char *text = selection_text(e);
  if (text == NULL)
    return; // nothing selected, or out of memory

  clipboard_set(text);
}

static void clip_cut(editor *e) {
  char *text = selection_text(e);
  if (text == NULL)
    return;

  clipboard_set(text);
  selection_delete(e);

  e->dirty = 1;
  dispatch_change(e);
}

static void clip_paste(editor *e) {
  // The system clipboard wins, the internal register is the fallback. An
  // empty string means the helper ran but had nothing to hand over, so it
  // counts as no answer at all.
  char *from_system = system_clipboard_paste();
  if (from_system != NULL && from_system[0] == '\0') {
    free(from_system);
    from_system = NULL;
  }

  const char *text =
      (from_system != NULL) ? from_system : internal_clipboard;

  if (text == NULL || text[0] == '\0') {
    free(from_system); // a no-op when it is NULL, which is the case here
    return;
  }

  // Pasting over a selection replaces it.
  if (e->sel_active)
    selection_delete(e);

  insert_text_at_cursor(e, text);
  free(from_system); // the internal register is not ours to free here

  e->dirty = 1;
  cursor_mark_column(e);
  dispatch_change(e);
}

///////////////////////////////////
// MODULE

static void clip_init(editor *e) {
  (void)e;

  clipboard_platform_init();

  dispatch_bind(CTRL('c'), clip_copy);
  dispatch_bind(CTRL('x'), clip_cut);
  dispatch_bind(CTRL('v'), clip_paste);
}

static void clip_shutdown(editor *e) {
  (void)e;

  free(internal_clipboard);
  internal_clipboard = NULL;
}

static module clipboard = {
  .name = "clipboard",
  .init = clip_init,
  .on_key = NULL,
  .on_draw = NULL,
  .on_change = NULL,
  .shutdown = clip_shutdown
};

module *clipboard_module(void) {
  return &clipboard;
}
