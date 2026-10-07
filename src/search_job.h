#ifndef SEARCH_JOB_H
#define SEARCH_JOB_H
#include "buffer.h"
#include "pattern.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
typedef struct search_job search_job;
typedef struct search_hit {
  size_t file;
  int row, col, length;
} search_hit;
typedef struct search_file {
  char *path;
  uint64_t first, count;
  struct stat stamp;
} search_file;
typedef struct search_progress {
  uint64_t matches, scanned, skipped, changed;
  size_t files;
  int done, failed, workers;
  char error[160];
} search_progress;
search_job *search_job_start(const search_pattern *pattern,
                             const buffer *buffer, const char *root,
                             const char *replacement);
// Explicit scanner count for benchmarks; 0 chooses the automatic default.
search_job *search_job_start_threads(const search_pattern *pattern,
                                     const char *root, int threads);
search_job *search_job_replace(search_job *completed, const char *replacement);
void search_job_cancel(search_job *job, int wait);
void search_job_stop(
    search_job *job); // CPU-only buffer jobs stop before returning.
void search_job_progress(search_job *job, search_progress *progress);
size_t search_job_hits(search_job *job, uint64_t index, search_hit *hits,
                       size_t capacity);
int search_job_hit(search_job *job, uint64_t index, search_hit *hit,
                   char **path);
int search_job_file(search_job *job, size_t index, search_file *file);
#endif
