#ifndef PROCESS_H
#define PROCESS_H

#include <stddef.h>
#include <stdint.h>

typedef struct process_stream {
  char *data;
  size_t length, capacity;
  intptr_t handle;
  int open;
} process_stream;

typedef struct child_process {
  process_stream out, err, in;
  void *input_state;
  int duplex;
  intptr_t handle, id, group;
  uint64_t started, cancel_started;
  int running, exit_code, failed, cancelled;
} child_process;

int process_start(child_process *p, const char *const argv[], const char *const env[]);
int process_start_duplex(child_process *p, const char *const argv[]);
int process_send(child_process *p, const char *data, size_t length);
void process_consume(process_stream *stream, size_t bytes);
int process_poll(child_process *p);
void process_cancel(child_process *p);
void process_dispose(child_process *p);

#endif
