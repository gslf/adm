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

#endif
