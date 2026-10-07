#include "search_job.h"
#include "fileio.h"
#include "path.h"
#include "utf8.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <windows.h>
#define seek _fseeki64
#else
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#define seek fseeko
#endif
#define HIT_BATCH 256
#define FILE_QUEUE 256
#define MAX_WORKERS 32
typedef struct search_pool search_pool;
struct search_job {
  search_pattern pattern;
  struct stat stamp;
  search_job *source, *owner;
  search_pool *pool;
  int workers;
  const buffer *buffer;
  char *root, *replacement;
  FILE *spool;
  atomic_int cancel, done, references;
#ifdef _WIN32
  CRITICAL_SECTION mutex;
#else
  pthread_mutex_t mutex;
#endif
  search_progress progress;
  search_file *files;
  size_t capacity;
  search_hit batch[HIT_BATCH];
  size_t pending;
};
static void lock(search_job *j) {
#ifdef _WIN32
  EnterCriticalSection(&j->mutex);
#else
  pthread_mutex_lock(&j->mutex);
#endif
}
static void unlock(search_job *j) {
#ifdef _WIN32
  LeaveCriticalSection(&j->mutex);
#else
  pthread_mutex_unlock(&j->mutex);
#endif
}
static const atomic_int *cancel_flag(search_job *j) {
  return j->owner ? &j->owner->cancel : &j->cancel;
}
static int cancelled(search_job *j) { return atomic_load(cancel_flag(j)); }
static char *copy(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = malloc(n);
  if (p)
    memcpy(p, s, n);
  return p;
}
static void release(search_job *j) {
  if (atomic_fetch_sub(&j->references, 1) != 1)
    return;
  for (size_t i = 0; i < j->progress.files; i++)
    free(j->files[i].path);
  free(j->files);
  free(j->root);
  free(j->replacement);
  if (j->source)
    release(j->source);
  if (j->spool)
    fclose(j->spool);
#ifdef _WIN32
  DeleteCriticalSection(&j->mutex);
#else
  pthread_mutex_destroy(&j->mutex);
#endif
  free(j);
}
static void failure(search_job *j, const char *message) {
  if (j->owner)
    j = j->owner;
  lock(j);
  j->progress.failed = 1;
  snprintf(j->progress.error, sizeof j->progress.error, "%s", message);
  unlock(j);
  atomic_store(&j->cancel, 1);
}
static int flush(search_job *j) {
  if (!j->pending)
    return 1;
  lock(j);
  if (!j->spool)
    j->spool = tmpfile();
  int ok =
      j->spool && seek(j->spool, 0, SEEK_END) == 0 &&
      fwrite(j->batch, sizeof *j->batch, j->pending, j->spool) == j->pending &&
      fflush(j->spool) == 0;
  if (ok) {
    j->progress.matches += j->pending;
    j->files[j->batch[0].file].count += j->pending;
  }
  unlock(j);
  j->pending = 0;
  if (!ok)
    failure(j, "Cannot store search results (temporary disk full?)");
  return ok;
}
static size_t add_file(search_job *j, const char *path) {
  char *name = copy(path);
  if (!name) {
    failure(j, "Out of memory");
    return SIZE_MAX;
  }
  lock(j);
  size_t at = j->progress.files;
  if (at == j->capacity) {
    size_t cap = j->capacity ? j->capacity * 2 : 32;
    search_file *p = realloc(j->files, cap * sizeof *p);
    if (!p) {
      unlock(j);
      free(name);
      failure(j, "Out of memory");
      return SIZE_MAX;
    }
    j->files = p;
    j->capacity = cap;
  }
  j->files[at] = (search_file){
      .path = name, .first = j->progress.matches, .stamp = j->stamp};
  j->progress.files++;
  unlock(j);
  return at;
}
static int add_hit(search_job *j, size_t file, int row, int col, int length) {
  j->batch[j->pending++] = (search_hit){file, row, col, length};
  return j->pending < HIT_BATCH || flush(j);
}
static int scan_line(search_job *j, const char *line, int size, int row,
                     const char *path, size_t *file) {
  for (int from = 0; from <= size && !cancelled(j);) {
    int length, x = pattern_find(&j->pattern, line, size, from, &length,
                                 cancel_flag(j));
    if (x < 0)
      break;
    if (*file == SIZE_MAX) {
      *file = add_file(j, path);
      if (*file == SIZE_MAX)
        return 0;
    }
    if (!add_hit(j, *file, row, x, length))
      return 0;
    // Publish the first result immediately, then batch the rest.
    if (!j->owner && j->progress.matches == 0 && !flush(j))
      return 0;
    if (length)
      from = x + length;
    else if (x == size)
      break;
    else {
      int cp;
      from = x + utf8_decode(line + x, &cp);
    }
  }
  return !cancelled(j);
}
static void scan_buffer(search_job *j) {
  int row = 0;
  size_t file = SIZE_MAX;
  for (block *b = j->buffer->head; b && !cancelled(j); b = b->next)
    for (int i = 0; i < b->count && !cancelled(j); i++, row++)
      if (!scan_line(j, b->lines[i], (int)strlen(b->lines[i]), row, "", &file))
        return;
  flush(j);
}
// Streaming readers retain one line, rather than an entire workspace file.
// Regular files only; symlinks/reparse points and special devices are excluded.
static FILE *regular_file(const char *path, struct stat *info) {
#ifdef _WIN32
  DWORD attrs = GetFileAttributesA(path);
  if (attrs == INVALID_FILE_ATTRIBUTES ||
      attrs & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
    return NULL;
  FILE *fp = fopen(path, "rb");
  // Use the MinGW stat/fstat pair: _fstat requires a different CRT struct.
  if (fp && (fstat(_fileno(fp), info) < 0 ||
             (info->st_mode & _S_IFMT) != _S_IFREG)) {
    fclose(fp);
    fp = NULL;
  }
#else
  int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return NULL;
  if (fstat(fd, info) < 0 || !S_ISREG(info->st_mode)) {
    close(fd);
    return NULL;
  }
  FILE *fp = fdopen(fd, "rb");
  if (!fp)
    close(fd);
#endif
  return fp;
}
static int same_file(const struct stat *a, const struct stat *b) {
  return a->st_size == b->st_size && a->st_mtime == b->st_mtime &&
         a->st_ctime == b->st_ctime && a->st_dev == b->st_dev &&
         a->st_ino == b->st_ino
#ifndef _WIN32
#ifdef __APPLE__
         && a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
         a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec
#else
         && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
         a->st_ctim.tv_nsec == b->st_ctim.tv_nsec
#endif
#endif
      ;
}
// Write to an exclusive temporary sibling and only commit after the source
// still has the same identity and timestamps. Cancellation discards this file.
static FILE *replacement_file(const char *path, char **temporary,
                              const struct stat *source) {
  *temporary = malloc(strlen(path) + 24);
  if (!*temporary)
    return NULL;
#ifdef _WIN32
  (void)source;
  snprintf(*temporary, strlen(path) + 24, "%s.adm-search-%lu", path,
           (unsigned long)GetCurrentThreadId());
  return fopen(*temporary, "wbx");
#else
  snprintf(*temporary, strlen(path) + 24, "%s.adm-search-XXXXXX", path);
  int fd = mkstemp(*temporary);
  if (fd < 0)
    return NULL;
  if (fchmod(fd, source->st_mode & 0777) < 0) {
    close(fd);
    remove(*temporary);
    return NULL;
  }
  FILE *fp = fdopen(fd, "wb");
  if (!fp) {
    close(fd);
    remove(*temporary);
  }
  return fp;
#endif
}
static int replace_line(search_job *j, FILE *out, const char *line, int size,
                        uint64_t *changed) {
  int from = 0, written = 0;
  while (from <= size && !cancelled(j)) {
    int length, x = pattern_find(&j->pattern, line, size, from, &length,
                                 cancel_flag(j));
    if (x < 0)
      break;
    size_t n = (size_t)(x - written), r = strlen(j->replacement);
    if (fwrite(line + written, 1, n, out) != n ||
        fwrite(j->replacement, 1, r, out) != r)
      return 0;
    written = x + length;
    (*changed)++;
    if (length)
      from = written;
    else if (x == size)
      break;
    else {
      int cp;
      from = x + utf8_decode(line + x, &cp);
    }
  }
  size_t n = (size_t)(size - written);
  return !cancelled(j) && fwrite(line + written, 1, n, out) == n;
}
static void scan_file(search_job *j, const char *path,
                      const struct stat *expected) {
  struct stat original;
  FILE *fp = regular_file(path, &original);
  if (!fp) {
    lock(j);
    j->progress.skipped++;
    unlock(j);
    return;
  }
  if ((j->replacement && original.st_nlink > 1) ||
      (expected && !same_file(&original, expected))) {
    fclose(fp);
    lock(j);
    j->progress.skipped++;
    unlock(j);
    return;
  }
  j->stamp = original;
  char chunk[65536], *line = NULL, *temporary = NULL;
  size_t length = 0, capacity = 0, n, file = SIZE_MAX;
  uint64_t initial;
  lock(j);
  initial = j->progress.matches;
  unlock(j);
  int row = 0, ok = 1, binary = 0, first = 1;
  uint64_t changed = 0;
  FILE *out =
      j->replacement ? replacement_file(path, &temporary, &original) : NULL;
  if (j->replacement && !out)
    ok = 0;
  while (ok && !cancelled(j) && (n = fread(chunk, 1, sizeof chunk, fp)) > 0) {
    if (memchr(chunk, 0, n)) {
      binary = 1;
      ok = 0;
      break;
    }
    for (size_t offset = 0; offset < n && ok;) {
      if (cancelled(j)) {
        ok = 0;
        break;
      }
      const char *newline = memchr(chunk + offset, '\n', n - offset);
      size_t bytes =
          (size_t)((newline ? newline : chunk + n) - (chunk + offset));
      if (!out && !length && newline) {
        int cr = bytes && chunk[offset + bytes - 1] == '\r';
        int bom =
            first && bytes >= 3 && !memcmp(chunk + offset, "\xef\xbb\xbf", 3)
                ? 3
                : 0;
        ok = scan_line(j, chunk + offset + bom, (int)bytes - cr - bom, row,
                       path, &file);
        offset += bytes + 1;
        first = 0;
        if (row == INT_MAX)
          ok = 0;
        else
          row++;
        continue;
      }
      if (bytes > (size_t)INT_MAX - 1 - length) {
        ok = 0;
        break;
      }
      size_t needed = length + bytes + 1;
      if (needed > capacity) {
        size_t cap = capacity ? capacity : 4096;
        while (cap < needed)
          cap *= 2;
        char *more = realloc(line, cap);
        if (!more) {
          failure(j, "Out of memory");
          ok = 0;
          break;
        }
        line = more;
        capacity = cap;
      }
      memcpy(line + length, chunk + offset, bytes);
      length += bytes;
      line[length] = 0;
      offset += bytes;
      if (!newline)
        break;
      offset++;
      int cr = length && line[length - 1] == '\r';
      int bom =
          first && length >= 3 && !memcmp(line, "\xef\xbb\xbf", 3) ? 3 : 0;
      int size = (int)length - cr - bom;
      if (out) {
        if (bom && fwrite(line, 1, 3, out) != 3)
          ok = 0;
        if (ok)
          ok = replace_line(j, out, line + bom, size, &changed);
        if (ok &&
            (cr ? fwrite("\r\n", 1, 2, out) != 2 : fputc('\n', out) == EOF))
          ok = 0;
      } else
        ok = scan_line(j, line + bom, size, row, path, &file);
      length = 0;
      first = 0;
      if (row == INT_MAX)
        ok = 0;
      else
        row++;
    }
  }
  if (ok && length && !cancelled(j)) {
    line[length] = 0;
    int bom = first && length >= 3 && !memcmp(line, "\xef\xbb\xbf", 3) ? 3 : 0;
    int cr = length && line[length - 1] == '\r';
    if (out) {
      if (bom && fwrite(line, 1, 3, out) != 3)
        ok = 0;
      if (ok)
        ok = replace_line(j, out, line + bom, (int)length - cr - bom, &changed);
      if (ok && cr && fputc('\r', out) == EOF)
        ok = 0;
    } else
      ok = scan_line(j, line + bom, (int)length - cr - bom, row, path, &file);
  }
  if (ferror(fp))
    ok = 0;
  struct stat after;
#ifdef _WIN32
  if (fstat(_fileno(fp), &after) < 0 || !same_file(&original, &after))
    ok = 0;
#else
  if (fstat(fileno(fp), &after) < 0 || !same_file(&original, &after))
    ok = 0;
#endif
  fclose(fp);
  free(line);
  if (out) {
    if (fflush(out) != 0)
      ok = 0;
#ifndef _WIN32
    if (ok && changed && fsync(fileno(out)) < 0)
      ok = 0;
#endif
    if (fclose(out) != 0)
      ok = 0;
    if (ok && changed && !cancelled(j)) {
#ifdef _WIN32
      if (stat(path, &after) < 0 || !same_file(&original, &after) ||
          !MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING))
        ok = 0;
#else
      if (lstat(path, &after) < 0 || !same_file(&original, &after) ||
          rename(temporary, path) < 0)
        ok = 0;
#endif
      if (ok) {
        lock(j);
        j->progress.changed++;
        j->progress.matches += changed;
        unlock(j);
      }
    }
    remove(temporary);
  } else if (binary || !ok) {
    // Discard all provisional matches if binary content or an unstable read
    // appears later in the file; previously completed files remain valid.
    j->pending = 0;
    lock(j);
    int discarded = !j->spool || fflush(j->spool) == 0;
    if (file != SIZE_MAX) {
      free(j->files[file].path);
      j->progress.files--;
      if (j->spool) {
#ifdef _WIN32
        discarded &=
            _chsize_s(_fileno(j->spool), initial * sizeof(search_hit)) == 0;
#else
        discarded &= ftruncate(fileno(j->spool),
                               (off_t)(initial * sizeof(search_hit))) == 0;
#endif
      }
      j->progress.matches = initial;
    }
    unlock(j);
    if (!discarded)
      failure(j, "Cannot discard incomplete search results");
  } else if (!j->owner)
    flush(j);
  free(temporary);
  lock(j);
  j->progress.scanned++;
  if (!ok && !cancelled(j))
    j->progress.skipped++;
  unlock(j);
}
// The traversal producer and a bounded queue are separate from scanners.
// Workers never hold the queue mutex while doing I/O or publishing results.
struct search_pool {
  search_job *job;
  char *paths[FILE_QUEUE];
  size_t head, count;
  int finished;
#ifdef _WIN32
  CRITICAL_SECTION mutex, publish;
  CONDITION_VARIABLE changed;
#else
  pthread_mutex_t mutex, publish;
  pthread_cond_t changed;
#endif
};
static void pool_lock(search_pool *p) {
#ifdef _WIN32
  EnterCriticalSection(&p->mutex);
#else
  pthread_mutex_lock(&p->mutex);
#endif
}
static void pool_unlock(search_pool *p) {
#ifdef _WIN32
  LeaveCriticalSection(&p->mutex);
#else
  pthread_mutex_unlock(&p->mutex);
#endif
}
static void pool_signal(search_pool *p) {
#ifdef _WIN32
  WakeAllConditionVariable(&p->changed);
#else
  pthread_cond_broadcast(&p->changed);
#endif
}
static void pool_wait(search_pool *p) {
  // Public cancellation only touches an atomic flag, so it cannot race pool
  // destruction. Timed waits also unblock saturated queues on cancellation.
#ifdef _WIN32
  SleepConditionVariableCS(&p->changed, &p->mutex, 25);
#else
  struct timespec until;
  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_nsec += 25000000;
  if (until.tv_nsec >= 1000000000) {
    until.tv_sec++;
    until.tv_nsec -= 1000000000;
  }
  pthread_cond_timedwait(&p->changed, &p->mutex, &until);
#endif
}
static void submit_file(search_job *j, const char *path) {
  if (!j->pool) {
    scan_file(j, path, NULL);
    return;
  }
  char *owned = copy(path);
  if (!owned) {
    failure(j, "Out of memory");
    return;
  }
  search_pool *p = j->pool;
  pool_lock(p);
  while (p->count == FILE_QUEUE && !cancelled(j))
    pool_wait(p);
  if (cancelled(j))
    free(owned);
  else {
    p->paths[(p->head + p->count) % FILE_QUEUE] = owned;
    p->count++;
    pool_signal(p);
  }
  pool_unlock(p);
}
// DFS uses an explicit queue. No recursion limit, symlink loops or directory
// handles held while processing descendants. .git metadata is never text.
static void scan_workspace(search_job *j) {
  char **dirs = NULL;
  size_t count = 0, capacity = 0;
  char *initial = copy(j->root);
  if (!initial) {
    failure(j, "Out of memory");
    return;
  }
  capacity = 32;
  dirs = malloc(capacity * sizeof *dirs);
  if (!dirs) {
    free(initial);
    failure(j, "Out of memory");
    return;
  }
  dirs[count++] = initial;
  while (count && !cancelled(j)) {
    char *directory = dirs[--count];
#ifdef _WIN32
    char *glob = path_join(directory, "*");
    WIN32_FIND_DATAA entry;
    HANDLE h = glob ? FindFirstFileA(glob, &entry) : INVALID_HANDLE_VALUE;
    free(glob);
    if (h != INVALID_HANDLE_VALUE)
      do {
        const char *name = entry.cFileName;
        if (!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, ".git"))
          continue;
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
          continue;
        char *path = path_join(directory, name);
        int isdir = !!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY),
            regular = !isdir;
#else
    DIR *h = opendir(directory);
    struct dirent *entry;
    if (h)
      while ((entry = readdir(h)) && !cancelled(j)) {
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..") || !strcmp(name, ".git"))
          continue;
        char *path = path_join(directory, name);
        int isdir = 0, regular = 0;
#ifdef DT_REG
        if (entry->d_type != DT_UNKNOWN) {
          isdir = entry->d_type == DT_DIR;
          regular = entry->d_type == DT_REG;
        } else
#endif
        {
          struct stat info;
          if (path && lstat(path, &info) == 0) {
            isdir = S_ISDIR(info.st_mode);
            regular = S_ISREG(info.st_mode);
          }
        }
#endif
        if (!path) {
          failure(j, "Out of memory");
          break;
        }
        if (isdir) {
          if (count == capacity) {
            size_t cap = capacity * 2;
            char **more = realloc(dirs, cap * sizeof *more);
            if (!more) {
              free(path);
              failure(j, "Out of memory");
              break;
            }
            dirs = more;
            capacity = cap;
          }
          dirs[count++] = path;
        } else {
          if (regular)
            submit_file(j, path);
          free(path);
        }
#ifdef _WIN32
      } while (!cancelled(j) && FindNextFileA(h, &entry));
    if (h != INVALID_HANDLE_VALUE)
      FindClose(h);
#else
      }
    if (h)
      closedir(h);
#endif
    if (
#ifdef _WIN32
        h == INVALID_HANDLE_VALUE
#else
        !h
#endif
    ) {
      lock(j);
      j->progress.skipped++;
      unlock(j);
    }
    free(directory);
  }
  while (count)
    free(dirs[--count]);
  free(dirs);
}
static search_job *scratch_job(search_job *owner) {
  search_job *s = calloc(1, sizeof *s);
  if (!s)
    return NULL;
  s->pattern = owner->pattern;
  s->owner = owner;
  atomic_init(&s->references, 1);
  atomic_init(&s->cancel, 0);
  atomic_init(&s->done, 0);
#ifdef _WIN32
  InitializeCriticalSection(&s->mutex);
#else
  if (pthread_mutex_init(&s->mutex, NULL)) {
    free(s);
    return NULL;
  }
#endif
  return s;
}
static void publish_file(search_pool *p, search_job *s) {
  search_job *j = p->job;
#ifdef _WIN32
  EnterCriticalSection(&p->publish);
#else
  pthread_mutex_lock(&p->publish);
#endif
  // All hits of a file remain contiguous and in row/column order. Only the
  // short index batches lock the UI's mutex. Invalid/binary files stay local.
  if (s->progress.files && !cancelled(j)) {
    j->stamp = s->files[0].stamp;
    size_t file = add_file(j, s->files[0].path);
    if (file != SIZE_MAX) {
      if (s->spool)
        rewind(s->spool);
      uint64_t left = s->progress.matches;
      while (left && !cancelled(j)) {
        size_t n = left < HIT_BATCH ? (size_t)left : HIT_BATCH;
        if (fread(j->batch, sizeof *j->batch, n, s->spool) != n) {
          failure(j, "Cannot read worker search index");
          break;
        }
        for (size_t i = 0; i < n; i++)
          j->batch[i].file = file;
        j->pending = n;
        if (!flush(j))
          break;
        left -= n;
      }
      // Small result sets never touch a per-worker temporary file.
      if (s->pending && !cancelled(j)) {
        memcpy(j->batch, s->batch, s->pending * sizeof *s->batch);
        for (size_t i = 0; i < s->pending; i++)
          j->batch[i].file = file;
        j->pending = s->pending;
        flush(j);
      }
    }
  }
  lock(j);
  j->progress.scanned += s->progress.scanned;
  j->progress.skipped += s->progress.skipped;
  unlock(j);
#ifdef _WIN32
  LeaveCriticalSection(&p->publish);
#else
  pthread_mutex_unlock(&p->publish);
#endif
}
static int reset_scratch(search_job *s) {
  for (size_t i = 0; i < s->progress.files; i++)
    free(s->files[i].path);
  s->progress.files = 0;
  if (s->spool && s->progress.matches) {
#ifdef _WIN32
    if (_chsize_s(_fileno(s->spool), 0))
      return 0;
#else
    if (ftruncate(fileno(s->spool), 0))
      return 0;
#endif
    rewind(s->spool);
  }
  memset(&s->progress, 0, sizeof s->progress);
  s->pending = 0;
  return 1;
}
#ifdef _WIN32
static unsigned __stdcall scan_worker(void *arg)
#else
static void *scan_worker(void *arg)
#endif
{
  search_pool *p = arg;
  search_job *j = p->job, *s = scratch_job(j);
  if (!s)
    failure(j, "Cannot allocate search worker");
  while (s && !cancelled(j)) {
    pool_lock(p);
    while (!p->count && !p->finished && !cancelled(j))
      pool_wait(p);
    char *path = NULL;
    if (p->count && !cancelled(j)) {
      path = p->paths[p->head];
      p->head = (p->head + 1) % FILE_QUEUE;
      p->count--;
      pool_signal(p);
    }
    pool_unlock(p);
    if (!path)
      break;
    scan_file(s, path, NULL);
    free(path);
    publish_file(p, s);
    if (!reset_scratch(s)) {
      failure(j, "Cannot reset worker search index");
      break;
    }
  }
  if (s)
    release(s);
#ifdef _WIN32
  return 0;
#else
  return NULL;
#endif
}
static void parallel_workspace(search_job *j) {
  search_pool p = {.job = j};
#ifdef _WIN32
  InitializeCriticalSection(&p.mutex);
  InitializeCriticalSection(&p.publish);
  InitializeConditionVariable(&p.changed);
  HANDLE threads[MAX_WORKERS];
#else
  if (pthread_mutex_init(&p.mutex, NULL)) {
    scan_workspace(j);
    return;
  }
  if (pthread_mutex_init(&p.publish, NULL)) {
    pthread_mutex_destroy(&p.mutex);
    scan_workspace(j);
    return;
  }
  if (pthread_cond_init(&p.changed, NULL)) {
    pthread_mutex_destroy(&p.publish);
    pthread_mutex_destroy(&p.mutex);
    scan_workspace(j);
    return;
  }
  pthread_t threads[MAX_WORKERS];
#endif
  int started = 0;
  for (; started < j->workers; started++) {
#ifdef _WIN32
    uintptr_t t = _beginthreadex(NULL, 0, scan_worker, &p, 0, NULL);
    if (!t)
      break;
    threads[started] = (HANDLE)t;
#else
    if (pthread_create(&threads[started], NULL, scan_worker, &p))
      break;
#endif
  }
  lock(j);
  j->progress.workers = started ? started : 1;
  unlock(j);
  j->pool = started ? &p : NULL;
  scan_workspace(j);
  pool_lock(&p);
  p.finished = 1;
  pool_signal(&p);
  pool_unlock(&p);
  for (int i = 0; i < started; i++) {
#ifdef _WIN32
    WaitForSingleObject(threads[i], INFINITE);
    CloseHandle(threads[i]);
#else
    pthread_join(threads[i], NULL);
#endif
  }
  j->pool = NULL;
  for (size_t i = 0; i < p.count; i++)
    free(p.paths[(p.head + i) % FILE_QUEUE]);
#ifdef _WIN32
  DeleteCriticalSection(&p.publish);
  DeleteCriticalSection(&p.mutex);
#else
  pthread_cond_destroy(&p.changed);
  pthread_mutex_destroy(&p.publish);
  pthread_mutex_destroy(&p.mutex);
#endif
}
static int worker_count(int requested) {
  if (!requested) {
    const char *env = getenv("ADM_SEARCH_THREADS");
    if (env) {
      char *end;
      long n = strtol(env, &end, 10);
      if (*env && !*end && n >= 1 && n <= MAX_WORKERS)
        requested = (int)n;
    }
  }
  if (!requested) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    long cores = info.dwNumberOfProcessors;
#else
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    requested = cores > 1 ? (int)(cores > 8 ? 8 : cores - 1) : 1;
  }
  return requested < 1 ? 1 : requested > MAX_WORKERS ? MAX_WORKERS : requested;
}
#ifdef _WIN32
static unsigned __stdcall worker(void *arg)
#else
static void *worker(void *arg)
#endif
{
  search_job *j = arg;
  if (j->buffer)
    scan_buffer(j);
  else if (j->source) {
    search_progress progress;
    search_job_progress(j->source, &progress);
    for (size_t i = 0; i < progress.files && !cancelled(j); i++) {
      search_file f;
      if (!search_job_file(j->source, i, &f)) {
        failure(j, "Cannot read replacement targets");
        break;
      }
      scan_file(j, f.path, &f.stamp);
      free(f.path);
    }
  } else if (!j->replacement && j->workers > 1)
    parallel_workspace(j);
  else
    scan_workspace(j);
  atomic_store(&j->done, 1);
  release(j);
#ifdef _WIN32
  return 0;
#else
  return NULL;
#endif
}
static search_job *start_job(const search_pattern *pattern,
                             const buffer *buffer, const char *root,
                             const char *replacement, search_job *source,
                             int threads) {
  search_job *j = calloc(1, sizeof *j);
  if (!j)
    return NULL;
  j->workers = buffer || replacement ? 1 : worker_count(threads);
  j->progress.workers = 1;
  j->pattern = *pattern;
  j->buffer = buffer;
  if (source) {
    atomic_fetch_add(&source->references, 1);
    j->source = source;
  }
  atomic_init(&j->references, 1);
  atomic_init(&j->cancel, 0);
  atomic_init(&j->done, 0);
#ifdef _WIN32
  InitializeCriticalSection(&j->mutex);
#else
  if (pthread_mutex_init(&j->mutex, NULL)) {
    if (j->source)
      release(j->source);
    free(j);
    return NULL;
  }
#endif
  j->spool = tmpfile();
  j->root = root ? copy(root) : NULL;
  j->replacement = replacement ? copy(replacement) : NULL;
  if (!j->spool || (root && !j->root) || (replacement && !j->replacement)) {
    release(j);
    return NULL;
  }
  atomic_store(&j->references, 2);
#ifdef _WIN32
  uintptr_t thread = _beginthreadex(NULL, 0, worker, j, 0, NULL);
  if (!thread) {
    atomic_store(&j->references, 1);
    release(j);
    return NULL;
  }
  CloseHandle((HANDLE)thread);
#else
  pthread_t thread;
  if (pthread_create(&thread, NULL, worker, j)) {
    atomic_store(&j->references, 1);
    release(j);
    return NULL;
  }
  pthread_detach(thread);
#endif
  return j;
}
search_job *search_job_start(const search_pattern *pattern,
                             const buffer *buffer, const char *root,
                             const char *replacement) {
  return start_job(pattern, buffer, root, replacement, NULL, 0);
}
search_job *search_job_start_threads(const search_pattern *pattern,
                                     const char *root, int threads) {
  return start_job(pattern, NULL, root, NULL, NULL, threads);
}
search_job *search_job_replace(search_job *completed, const char *replacement) {
  search_progress p;
  search_job_progress(completed, &p);
  if (!completed || completed->buffer || !p.done || p.failed)
    return NULL;
  return start_job(&completed->pattern, NULL, completed->root, replacement,
                   completed, 1);
}
void search_job_cancel(search_job *j, int wait) {
  if (!j)
    return;
  atomic_store(&j->cancel, 1);
  // Memory searches borrow immutable editor text only while the prompt owns
  // input. Wait for cooperative CPU cancellation before releasing that text.
  if (wait)
    while (!atomic_load(&j->done)) {
#ifdef _WIN32
      Sleep(0);
#else
      sched_yield();
#endif
    }
}
void search_job_stop(search_job *j) {
  if (!j)
    return;
  search_job_cancel(j, j->buffer != NULL || j->replacement != NULL);
  release(j);
}
void search_job_progress(search_job *j, search_progress *p) {
  memset(p, 0, sizeof *p);
  if (!j) {
    p->done = 1;
    return;
  }
  lock(j);
  *p = j->progress;
  unlock(j);
  p->done = atomic_load(&j->done);
  if (j->source) {
    search_progress source;
    search_job_progress(j->source, &source);
    p->files = source.files;
  }
}
int search_job_hit(search_job *j, uint64_t index, search_hit *hit,
                   char **path) {
  if (!j)
    return 0;
  lock(j);
  int available = index < j->progress.matches;
  int ok = available && index < INT64_MAX / sizeof *hit &&
           seek(j->spool, (int64_t)(index * sizeof *hit), SEEK_SET) == 0 &&
           fread(hit, sizeof *hit, 1, j->spool) == 1;
  int broken = available && !ok;
  if (ok && path) {
    *path = copy(j->files[hit->file].path);
    ok = *path != NULL;
  }
  unlock(j);
  if (broken)
    failure(j, "Cannot read search index");
  return ok;
}
int search_job_file(search_job *j, size_t index, search_file *file) {
  if (j && j->source)
    return search_job_file(j->source, index, file);
  if (!j)
    return 0;
  lock(j);
  int ok = index < j->progress.files;
  if (ok) {
    *file = j->files[index];
    file->path = copy(file->path);
    ok = file->path != NULL;
  }
  unlock(j);
  return ok;
}

size_t search_job_hits(search_job *j, uint64_t index, search_hit *hits,
                       size_t capacity) {
  if (!j || index >= INT64_MAX / sizeof *hits)
    return 0;
  lock(j);
  uint64_t available =
      index < j->progress.matches ? j->progress.matches - index : 0;
  size_t n = available < capacity ? (size_t)available : capacity;
  size_t expected = n;
  if (n && seek(j->spool, (int64_t)(index * sizeof *hits), SEEK_SET) == 0)
    n = fread(hits, sizeof *hits, n, j->spool);
  else
    n = 0;
  unlock(j);
  if (n != expected)
    failure(j, "Cannot read search index");
  return n;
}
