#include "process.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
static void pause_tick(void) { Sleep(1); }
#else
#include <unistd.h>
static void pause_tick(void) { usleep(1000); }
#endif

static void settle(child_process *process) {
  for (int i = 0; i < 10000; i++) {
    if (process_poll(process))
      return;
    pause_tick();
  }
  assert(!"Process failed to finish");
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "--child")) {
    assert(argc == 4 && getchar() == EOF);
    assert(!strcmp(getenv("ADM_PROCESS_TEST"), "child only"));
    fwrite(argv[2], 1, strlen(argv[2]) + 1, stdout);
    fwrite(argv[3], 1, strlen(argv[3]) + 1, stdout);
    fputs("separate stderr\n", stderr);
    return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "--echo")) {
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, stdin))) {
      assert(fwrite(chunk, 1, n, stdout) == n);
      fflush(stdout);
    }
    return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "--wait")) {
    for (int i = 0; i < 2000; i++)
      pause_tick();
    return 0;
  }
  if (argc > 1 && !strcmp(argv[1], "--large")) {
    char chunk[4096];
    memset(chunk, 'x', sizeof chunk);
    for (int i = 0; i < 2300; i++)
      assert(fwrite(chunk, 1, sizeof chunk, stdout) == sizeof chunk);
    return 0;
  }
  child_process process = {0};
  const char *args[] = {argv[0], "--child", "space \"quote\" $() ; trailing\\",
                        "", NULL};
  const char *env[] = {"ADM_PROCESS_TEST=child only", NULL};
  assert(process_start(&process, args, env) == 0);
  settle(&process);
  assert(process.exit_code == 0 && !process.failed && !process.cancelled);
  assert(process.out.length == strlen(args[2]) + 2 &&
         !strcmp(process.out.data, args[2]));
  assert(!strcmp(process.err.data, "separate stderr\n"));
  assert(!getenv("ADM_PROCESS_TEST"));
  process_dispose(&process);

  const char *echo[] = {argv[0], "--echo", NULL};
  assert(process_start_duplex(&process, echo) == 0);
  char input[4096];
  memset(input, 'y', sizeof input);
  for (int i = 0; i < 256; i++)
    assert(process_send(&process, input, sizeof input) == 0);
  for (int i = 0; i < 10000 && process.out.length < 1024 * 1024; i++) {
    process_poll(&process);
    pause_tick();
  }
  assert(process.out.length == 1024 * 1024 && !process.failed);
  for (size_t i = 0; i < process.out.length; i++)
    assert(process.out.data[i] == 'y');
  process_consume(&process.out, process.out.length);
  assert(!process.out.length && !process.out.data[0]);
  process_dispose(&process);

  const char *slow[] = {argv[0], "--wait", NULL};
  assert(process_start(&process, slow, NULL) == 0);
  assert(!process_poll(&process));
  process_cancel(&process);
  settle(&process);
  assert(process.cancelled && process.exit_code != 0);
  process_dispose(&process);

  const char *large[] = {argv[0], "--large", NULL};
  assert(process_start(&process, large, NULL) == 0);
  settle(&process);
  assert(process.failed && process.out.length <= 8 * 1024 * 1024);
  process_dispose(&process);
  assert(!process.running && !process.out.data && !process.err.data);
  return 0;
}
