#ifndef TERMINAL_H
#define TERMINAL_H

typedef struct {
  int rows;
  int cols;
} termsize;

int init_raw (void);
void restore();
termsize get_size(void);
int term_resized(void);
// 1 when input is buffered or arrives within the given time.
int term_wait(int milliseconds);
// Next input byte, waiting at most the given time; -1 when none arrives.
int term_read(int milliseconds);
// Give back the byte just read, so the next term_read returns it again.
void term_unread(void);
// Milliseconds on a monotonic clock, for timing keys against each other.
unsigned long long term_milliseconds(void);

#endif
