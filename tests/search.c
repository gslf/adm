#include "search.h"
#include "documents.h"
#include "fileio.h"
#include "path.h"
#include "screen.h"
#include "search_job.h"
#include "shared_views.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
static void pause_worker(void) {
#ifdef _WIN32
  Sleep(1);
#else
  usleep(1000);
#endif
}
static void prefix(editor *e, int key) {
  dispatch_key(e, CTRL('x'));
  dispatch_key(e, key);
}
static void type(editor *e, const char *text) {
  while (*text)
    dispatch_key(e, (unsigned char)*text++);
}
static unsigned long long wait_search(editor *e) {
  unsigned long long count = 0;
  int done = 0;
  for (int i = 0; i < 10000 && !done; i++) {
    search_status(e, &count, &done);
    pause_worker();
  }
  assert(done);
  return count;
}
static search_progress wait_job(search_job *j) {
  search_progress p = {0};
  for (int i = 0; i < 10000 && !p.done; i++) {
    search_job_progress(j, &p);
    pause_worker();
  }
  assert(p.done && !p.failed);
  return p;
}
static void rendered(editor *e, const char *text) {
  abuf ab = {0};
  dispatch_draw(e, &ab);
  ab_append(&ab, "\0", 1);
  if (!strstr(ab.b, text))
    fprintf(stderr, "Missing %s in %s\n", text, ab.b);
  assert(strstr(ab.b, text));
  ab_free(&ab);
}
static void parallel_regressions(const char *root) {
  char *directory = path_join(root, "parallel");
#ifdef _WIN32
  assert(CreateDirectoryA(directory, NULL));
#else
  assert(mkdir(directory, 0700) == 0);
#endif
  for (int i = 0; i < 600; i++) {
    char name[40];
    snprintf(name, sizeof name, "%d.txt", i);
    char *path = path_join(directory, name);
    assert(file_write(path, "parallelneedle x parallelneedle\n") == 0);
    free(path);
  }
  char *path = path_join(directory, "long.txt");
  FILE *f = fopen(path, "wb");
  assert(f);
  char *longline = malloc(131073);
  assert(longline);
  memset(longline, 'x', 131073);
  memcpy(longline + 65520, "parallelneedle", 14);
  memcpy(longline + 70000, "parallelneedle", 14);
  assert(fwrite(longline, 1, 131073, f) == 131073 && fclose(f) == 0);
  free(path);
  path = path_join(directory, "late-binary.txt");
  f = fopen(path, "wb");
  assert(f);
  memcpy(longline, "parallelneedle\n", 15);
  longline[131072] = 0;
  assert(fwrite(longline, 1, 131073, f) == 131073 && fclose(f) == 0);
  free(path);
  free(longline);
  path = path_join(directory, "unicode.txt");
  assert(file_write(path, "界 parallelneedle é\n") == 0);
  free(path);
  path = path_join(directory, "dense.txt");
  f = fopen(path, "wb");
  assert(f);
  for (int i = 0; i < 301; i++)
    assert(fputs("parallelneedle parallelneedle\n", f) >= 0);
  assert(fclose(f) == 0);
  free(path);
  search_pattern pattern;
  assert(pattern_compile(&pattern, "parallelneedle", 0));
  for (int threads = 1; threads <= 8; threads *= 2) {
    search_job *j = search_job_start_threads(&pattern, directory, threads);
    assert(j);
    search_progress p = wait_job(j);
    assert(p.matches == 1805 && p.files == 603 && p.skipped == 1);
    uint64_t next = 0;
    for (size_t i = 0; i < p.files; i++) {
      search_file file;
      assert(search_job_file(j, i, &file));
      assert(file.first == next && file.count);
      assert(!strstr(file.path, "binary"));
      search_hit previous = {.row = -1};
      for (uint64_t k = file.first; k < file.first + file.count; k++) {
        search_hit hit;
        assert(search_job_hit(j, k, &hit, NULL));
        assert(hit.file == i && hit.length == 14);
        assert(hit.row > previous.row ||
               (hit.row == previous.row && hit.col > previous.col));
        previous = hit;
      }
      next += file.count;
      free(file.path);
    }
    assert(next == p.matches);
    search_job_stop(j);
  }
  // Exercise cancellation with a full producer queue and repeated lifetimes.
  for (int i = 0; i < 12; i++) {
    search_job *j = search_job_start_threads(&pattern, directory, 2);
    assert(j);
    pause_worker();
    search_job_cancel(j, 1);
    search_progress p;
    search_job_progress(j, &p);
    assert(p.done && !p.failed);
    search_job_stop(j);
  }
  free(directory);
}
int main(int argc, char **argv) {
  assert(argc == 2);
  assert(file_write(argv[1], "aaaa a.c abc\nneedle needle\n") == 0);
  editor e = {
      .document.filename = argv[1], .rows = 45, .cols = 100, .running = 1};
  assert(load(&e.document.buf, argv[1]) == 0);
  dispatch_register(search_module());
  dispatch_init(&e);
  assert(test_shared_split(&e, LAYOUT_VERTICAL));
  e.sidebar.focused = 0;
  view *other = &e.windows.panes[1];
  other->cy = 0;
  other->cx = 4;
  prefix(&e, 's');
  type(&e, "a.c");
  assert(wait_search(&e) == 1);
  rendered(&e, "FILE TEXT");
  assert(e.view->cx == 5);
  dispatch_key(&e, '\t');
  assert(wait_search(&e) == 2);
  rendered(&e, "FILE REGEX");
  dispatch_key(&e, KEY_DOWN);
  assert(e.view->cx == 9);
  dispatch_key(&e, KEY_DOWN);
  assert(e.view->cx == 5);
  dispatch_key(&e, '\x1b');
  assert(!search_active(&e));
  prefix(&e, 'r');
  type(&e, "aa");
  assert(wait_search(&e) == 2);
  dispatch_key(&e, '\r');
  type(&e, "X");
  dispatch_key(&e, '\r');
  rendered(&e, "y:replace");
  dispatch_key(&e, 'a');
  for (int i = 0;
       i < 1000 && strcmp(buffer_line(&e.document.buf, 0), "XX a.c abc"); i++) {
    dispatch_tick(&e);
    pause_worker();
  }
  assert(!strcmp(buffer_line(&e.document.buf, 0), "XX a.c abc") &&
         e.document.dirty && other->cx == 2);
  wait_search(&e);
  dispatch_key(&e, '\x1b');
  assert(file_write_buffer(argv[1], &e.document.buf) == 0);
  e.document.dirty = 0;
#ifndef _WIN32
  assert(chdir("folder") == 0);
  file_manager_refresh(&e);
  assert(!strcmp(e.files.tree.entries[0].path, e.files.workspace_root));
#endif
  // Two-row filtered Explorer; recursive root does not follow symlink cycles.
  prefix(&e, 'S');
  type(&e, "needle");
  assert(wait_search(&e) == 5);
  rect area = {0, 1, (int)strlen(e.files.workspace_root) + 80, 30};
  abuf ab = {0};
  assert(search_files_draw(&e, &ab, area));
  ab_append(&ab, "\0", 1);
  assert(strstr(ab.b, "FILES * SEARCH") && strstr(ab.b, "nested.txt") &&
         strstr(ab.b, e.files.workspace_root));
  assert(!strstr(ab.b, "binary.bin") && !strstr(ab.b, "unrelated.txt"));
  ab_free(&ab);
  rendered(&e, "WORKSPACE TEXT");
  dispatch_key(&e, '\r');
  assert(e.view->doc);
  dispatch_key(&e, '\x1b');
#ifndef _WIN32
  assert(chdir(e.files.workspace_root) == 0);
#endif
  // Bulk replacement refuses unsaved buffers in any shared split.
  e.document.dirty = 1;
  prefix(&e, 'R');
  type(&e, "needle");
  wait_search(&e);
  dispatch_key(&e, '\r');
  type(&e, "found");
  dispatch_key(&e, '\r');
  dispatch_key(&e, 'a');
  assert(!e.confirmation && strstr(e.notice, "Save workspace buffers"));
  e.document.dirty = 0;
  dispatch_key(&e, 'a');
  assert(e.confirmation);
  dispatch_key(&e, 'y');
  for (int i = 0; i < 10000 && search_active(&e); i++) {
    dispatch_tick(&e);
    pause_worker();
  }
  if (search_active(&e) || !strstr(e.notice, "3 files changed"))
    fprintf(stderr, "Workspace replacement (active=%d): %s\n", search_active(&e), e.notice);
  assert(!search_active(&e) && strstr(e.notice, "3 files changed"));
  char *saved = file_read("folder/nested.txt");
  assert(saved && !strcmp(saved, "found\r\nfound"));
  free(saved);
  saved = file_read(".hidden.txt");
  assert(saved && !strcmp(saved, "\xef\xbb\xbf"
                                 "found\n"));
  free(saved);
  saved = file_read(".git/metadata");
  assert(saved && !strcmp(saved, "needle\n"));
  free(saved);
  assert(!strcmp(buffer_line(&e.document.buf, 1), "found found"));
  // Opening the first workspace result may select any completed file. Restore
  // this test's two-line document before testing file-local replacements.
  documents_open(&e, argv[1], NULL);
  assert(e.view->doc == &e.document);
  // Zero-width regex replacement terminates and empty replacement is deletion.
  prefix(&e, 'r');
  type(&e, "^");
  dispatch_key(&e, '\t');
  assert(wait_search(&e) == 2);
  dispatch_key(&e, '\r');
  type(&e, "!");
  dispatch_key(&e, '\r');
  dispatch_key(&e, 'a');
  for (int i = 0; i < 1000 && buffer_line(&e.document.buf, 1)[0] != '!'; i++) {
    dispatch_tick(&e);
    pause_worker();
  }
  assert(!strcmp(buffer_line(&e.document.buf, 1), "!found found"));
  wait_search(&e);
  dispatch_key(&e, '\x1b');
  prefix(&e, 'r');
  type(&e, "!");
  wait_search(&e);
  dispatch_key(&e, '\r');
  dispatch_key(&e, '\r');
  dispatch_key(&e, 'a');
  for (int i = 0; i < 1000 && buffer_line(&e.document.buf, 1)[0] == '!'; i++) {
    dispatch_tick(&e);
    pause_worker();
  }
  assert(!strcmp(buffer_line(&e.document.buf, 1), "found found"));
  wait_search(&e);
  dispatch_key(&e, '\x1b');
  dispatch_shutdown(&e);
  search_pattern p;
  assert(pattern_compile(&p, "found", 0));
  char *cwd = path_current_directory();
  search_job *j = search_job_start(&p, NULL, cwd, NULL);
  assert(j);
  search_progress progress = wait_job(j);
  assert(progress.matches == 5 && progress.skipped >= 1);
  search_job_stop(j);
  free(cwd);
  // A search snapshot cannot authorize overwriting a subsequently changed file.
  assert(file_write("stale.txt", "found\n") == 0);
#ifndef _WIN32
  assert(file_write("linked.txt", "found\n") == 0);
  assert(link("linked.txt", "linked-alias.txt") == 0);
#endif
  cwd = path_current_directory();
  j = search_job_start(&p, NULL, cwd, NULL);
  assert(j);
  wait_job(j);
  assert(file_write("stale.txt", "external edit with found\n") == 0);
  search_job *replacement = search_job_replace(j, "safe");
  assert(replacement);
  progress = wait_job(replacement);
  assert(progress.skipped >= 1);
  saved = file_read("stale.txt");
  assert(saved && !strcmp(saved, "external edit with found\n"));
  free(saved);
#ifndef _WIN32
  saved = file_read("linked.txt");
  assert(saved && !strcmp(saved, "found\n"));
  free(saved);
#endif
  search_job_stop(replacement);
  search_job_stop(j);
  free(cwd);
  // Explicit cancellation releases borrowed buffer memory before edits resume.
  buffer big;
  buffer_init(&big);
  char *line = malloc(8 * 1024 * 1024 + 1);
  assert(line);
  memset(line, 'a', 8 * 1024 * 1024);
  line[8 * 1024 * 1024] = 0;
  assert(buffer_load_text(&big, line) == 0);
  free(line);
  assert(pattern_compile(&p, "(a|aa)*b", 1));
  j = search_job_start(&p, &big, NULL, NULL);
  assert(j);
  pause_worker();
  search_job_stop(j);
  buffer_free(&big);
  cwd = path_current_directory();
  parallel_regressions(cwd);
  free(cwd);
  puts("Search regressions passed.");
  return 0;
}
