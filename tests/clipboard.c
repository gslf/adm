#include <assert.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

static char output[90000];
static size_t output_length, chunk_size;
static int calls, interrupt_at, stop_at, stop_error;

static ssize_t clipboard_write(int fd, const void *data, size_t length) {
  assert(fd == STDOUT_FILENO);
  calls++;
  assert(calls < 100000);
  if (calls == interrupt_at) {
    errno = EINTR;
    return -1;
  }
  if (calls == stop_at) {
    errno = stop_error;
    return stop_error ? -1 : 0;
  }
  if (chunk_size && length > chunk_size)
    length = chunk_size;
  assert(output_length + length < sizeof output);
  memcpy(output + output_length, data, length);
  output_length += length;
  output[output_length] = '\0';
  return (ssize_t)length;
}

// Exercise the production OSC 52 path without modifying the desktop clipboard.
#define write clipboard_write
#include "../src/clipboard.c"
#undef write

static void reset(void) {
  output_length = chunk_size = 0;
  calls = interrupt_at = stop_at = stop_error = 0;
  output[0] = '\0';
}

int main(void) {
  const char *sequence = "\x1b]52;c;w6g=\x07";
  reset();
  terminal_clipboard_copy("\xc3\xa8");
  assert(calls == 1 && !strcmp(output, sequence));
  reset();
  chunk_size = 2;
  interrupt_at = 1;
  terminal_clipboard_copy("\xc3\xa8");
  assert(!strcmp(output, sequence));
  reset();
  chunk_size = 3;
  interrupt_at = 2;
  terminal_clipboard_copy("\xc3\xa8");
  assert(!strcmp(output, sequence));
  reset();
  chunk_size = 2;
  stop_at = 2;
  stop_error = EPIPE;
  terminal_clipboard_copy("abc");
  assert(calls == 2 && output_length == 2);
  reset();
  stop_at = 1;
  terminal_clipboard_copy("abc");
  assert(calls == 1 && !output_length);
  reset();
  terminal_clipboard_copy("");
  char oversized[OSC52_MAX_INPUT + 2];
  memset(oversized, 'a', sizeof oversized - 1);
  oversized[sizeof oversized - 1] = '\0';
  terminal_clipboard_copy(oversized);
  assert(!calls);
  oversized[OSC52_MAX_INPUT] = '\0';
  chunk_size = 127;
  interrupt_at = 3;
  terminal_clipboard_copy(oversized);
  assert(output_length == sizeof OSC52_PREFIX - 1 + 4 * ((OSC52_MAX_INPUT + 2) / 3) + 1);
  assert(!memcmp(output, OSC52_PREFIX, sizeof OSC52_PREFIX - 1));
  assert(output[output_length - 1] == '\x07');
  return 0;
}
