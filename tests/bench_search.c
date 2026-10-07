// Standalone end-to-end benchmark: includes traversal and occurrence indexing.
#include "search_job.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
static double now(void) {
#ifdef _WIN32
  LARGE_INTEGER value, frequency;
  QueryPerformanceCounter(&value);
  QueryPerformanceFrequency(&frequency);
  return (double)value.QuadPart / (double)frequency.QuadPart;
#else
  struct timespec value;
  clock_gettime(CLOCK_MONOTONIC, &value);
  return (double)value.tv_sec + (double)value.tv_nsec / 1e9;
#endif
}
int main(int argc, char **argv) {
  if (argc != 5) {
    fprintf(stderr, "Usage: %s ROOT QUERY REGEX THREADS\n", argv[0]);
    return 2;
  }
  search_pattern pattern;
  if (!pattern_compile(&pattern, argv[2], atoi(argv[3])))
    return 2;
  double start = now();
#ifdef ADM_BENCH_BASELINE
  search_job *job = search_job_start(&pattern, NULL, argv[1], NULL);
#else
  search_job *job = search_job_start_threads(&pattern, argv[1], atoi(argv[4]));
#endif
  if (!job)
    return 2;
  search_progress progress;
  do {
    search_job_progress(job, &progress);
    if (!progress.done) {
#ifdef _WIN32
      Sleep(1);
#else
      usleep(100);
#endif
    }
  } while (!progress.done);
  double seconds = now() - start;
  printf("{\"seconds\":%.6f,\"matches\":%llu,\"files\":%zu,\"scanned\":%llu}\n",
         seconds, (unsigned long long)progress.matches, progress.files,
         (unsigned long long)progress.scanned);
  if (progress.failed)
    fprintf(stderr, "%s\n", progress.error);
  search_job_stop(job);
  return progress.failed ? 1 : 0;
}
