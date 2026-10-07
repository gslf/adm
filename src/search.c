#include "theme.h"
#include "search.h"
#include "cursor.h"
#include "path.h"
#include "screen.h"
#include "search_job.h"
#include "utf8.h"
#include "undo.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  QUERY,
  REPLACEMENT,
  RESULTS,
  ACTIONS,
  FILE_REPLACING,
  WORKSPACE_REPLACING
};
struct search_state {
  int active, goto_mode, workspace, replacing, regex, phase, valid;
  char query[PATTERN_MAX], replacement[PATTERN_MAX], confirmation[160];
  int length, replacement_length, input_cursor;
  int oy, ox, orowoff, ocoloff, osticky;
  view *origin;
  unsigned long revision;
  search_pattern pattern;
  search_job *job;
  search_progress progress;
  uint64_t selected, replace_index, replaced;
  size_t selected_file, file_offset;
  int path_offset, shown, initial, cancelled;
  search_hit pending, cache[256];
  uint64_t cache_first;
  size_t cache_count;
};
typedef struct search_state search_state;
static void show_hit(editor *e);
static void update(editor *e);
static void close_search(editor *e, int accept);
static void restore(editor *e) {
  search_state *s = e->search;
  if (e->view != s->origin || e->view->revision != s->revision)
    return;
  e->view->cy = s->oy;
  e->view->cx = s->ox;
  e->view->rowoff = s->orowoff;
  e->view->coloff = s->ocoloff;
  e->view->sticky = s->osticky;
  selection_clear(e);
}
static void stop(search_state *s) {
  search_job_stop(s->job);
  s->job = NULL;
}
static void goto_update(editor *e) {
  search_state *s = e->search;
  if (!s->length) {
    restore(e);
    return;
  }
  long last = e->view->doc->buf.nlines > 0 ? e->view->doc->buf.nlines - 1 : 0;
  long row = strtol(s->query, NULL, 10) - 1;
  e->view->cy = (int)(row < 0 ? 0 : row > last ? last : row);
  e->view->cx = 0;
  cursor_mark_column(e);
}
static void update(editor *e) {
  search_state *s = e->search;
  if (s->goto_mode) {
    goto_update(e);
    return;
  }
  stop(s);
  memset(&s->progress, 0, sizeof s->progress);
  s->progress.done = 1;
  s->selected = s->selected_file = s->file_offset = 0;
  s->path_offset = 0;
  s->shown = 0;
  s->initial = 1;
  s->cache_count = 0;
  s->valid = pattern_compile(&s->pattern, s->query, s->regex);
  if (!s->workspace)
    restore(e);
  if (!s->length || !s->valid)
    return;
  s->job =
      search_job_start(&s->pattern, s->workspace ? NULL : &e->view->doc->buf,
                       s->workspace ? e->files.workspace_root : NULL, NULL);
  if (!s->job) {
    s->progress.failed = s->progress.done = 1;
    snprintf(s->progress.error, sizeof s->progress.error,
             "Cannot start search");
  }
}
static void open_prompt(editor *e, int workspace, int replacing,
                        int goto_mode) {
  if (!workspace && !e->view->doc)
    return;
  if (!workspace && !goto_mode && e->git.action != GIT_IDLE) {
    snprintf(e->notice, sizeof e->notice,
             "Wait for the active Git operation before searching this buffer");
    return;
  }
  if (workspace && !e->files.workspace_root) {
    snprintf(e->notice, sizeof e->notice, "Cannot find workspace directory");
    return;
  }
  search_state *s = e->search;
  if (!s) {
    snprintf(e->notice, sizeof e->notice, "Cannot initialize search");
    return;
  }
  stop(s);
  memset(s, 0, sizeof *s);
  s->active = 1;
  s->workspace = workspace;
  s->replacing = replacing;
  s->goto_mode = goto_mode;
  s->valid = 1;
  s->progress.done = 1;
  s->origin = e->view;
  s->revision = e->view->revision;
  s->oy = e->view->cy;
  s->ox = e->view->cx;
  s->orowoff = e->view->rowoff;
  s->ocoloff = e->view->coloff;
  s->osticky = e->view->sticky;
  selection_clear(e);
  if (workspace)
    sidebar_show(e, SIDEBAR_FILES);
  else
    e->sidebar.focused = 0;
}
static void search_begin(editor *e) { open_prompt(e, 0, 0, 0); }
static void workspace_begin(editor *e) { open_prompt(e, 1, 0, 0); }
static void replace_begin(editor *e) { open_prompt(e, 0, 1, 0); }
static void workspace_replace_begin(editor *e) { open_prompt(e, 1, 1, 0); }
static void goto_begin(editor *e) { open_prompt(e, 0, 0, 1); }
static void close_search(editor *e, int accept) {
  search_state *s = e->search;
  if (s && s->phase == FILE_REPLACING) undo_group_end(e);
  if (s->job)
    search_job_progress(s->job, &s->progress);
  if (s->progress.failed)
    snprintf(e->notice, sizeof e->notice, "%s", s->progress.error);
  stop(s);
  if (!accept && !s->workspace && !s->replaced)
    restore(e);
  else
    selection_clear(e);
  s->active = 0;
}
static void select_hit(editor *e, const search_hit *h) {
  search_state *s = e->search;
  const char *line =
      e->view->doc ? buffer_line(&e->view->doc->buf, h->row) : NULL;
  if (!line) {
    snprintf(e->notice, sizeof e->notice,
             "Search result changed; search again");
    return;
  }
  int length, x = pattern_find(&s->pattern, line, (int)strlen(line), h->col,
                               &length, NULL);
  if (x != h->col || length != h->length) {
    snprintf(e->notice, sizeof e->notice,
             "Search result changed; search again");
    return;
  }
  e->view->cy = h->row;
  e->view->cx = h->col;
  e->view->sely = h->row;
  e->view->selx = h->col + h->length;
  e->view->sel_active = h->length > 0;
  e->view->sel_mode = 0;
  cursor_mark_column(e);
  s->shown = 1;
}
static void opened(editor *e, int result) {
  if (result > 0 && e->search && e->search->active)
    select_hit(e, &e->search->pending);
  else if (result == 0)
    snprintf(e->notice, sizeof e->notice, "Cannot open search result");
}
static void show_hit(editor *e) {
  search_state *s = e->search;
  search_hit h;
  char *path = NULL;
  if (!search_job_hit(s->job, s->selected, &h, &path))
    return;
  s->selected_file = h.file;
  if (!s->workspace)
    select_hit(e, &h);
  else {
    s->pending = h;
    documents_open(e, path, opened);
  }
  free(path);
}
static void move_hit(editor *e, int step, int open) {
  search_state *s = e->search;
  if (!s->job)
    return;
  search_job_progress(s->job, &s->progress);
  if (!s->progress.matches)
    return;
  if (step > 0)
    s->selected = s->selected + 1 < s->progress.matches ? s->selected + 1 : 0;
  else
    s->selected = s->selected ? s->selected - 1 : s->progress.matches - 1;
  search_hit h;
  if (search_job_hit(s->job, s->selected, &h, NULL))
    s->selected_file = h.file;
  if (open || !s->workspace)
    show_hit(e);
}
static int replace_one(editor *e) {
  search_state *s = e->search;
  if (!s->shown || !documents_editable(e)) {
    snprintf(e->notice, sizeof e->notice, "Select an editable match first");
    return 0;
  }
  search_hit h;
  char *path = NULL;
  if (!search_job_hit(s->job, s->selected, &h, &path))
    return 0;
  if (s->workspace && !document_matches(e->view->doc, path)) {
    free(path);
    snprintf(e->notice, sizeof e->notice,
             "Enter opens the selected match before replacing");
    return 0;
  }
  free(path);
  const char *line = buffer_line(&e->view->doc->buf, h.row);
  int length, x = line ? pattern_find(&s->pattern, line, (int)strlen(line),
                                      h.col, &length, NULL)
                       : -1;
  if (x != h.col || length != h.length) {
    snprintf(e->notice, sizeof e->notice,
             "Search result changed; search again");
    return 0;
  }
  // Stop memory readers before changing shared text. Workspace jobs read disk.
  if (!s->workspace)
    search_job_cancel(s->job, 1);
  int changed = strlen(s->replacement) != (size_t)h.length ||
                memcmp(line + h.col, s->replacement, (size_t)h.length);
  if (changed && buffer_replace_range(&e->view->doc->buf, h.row, h.col, h.length,
                           s->replacement) < 0) {
    snprintf(e->notice, sizeof e->notice,
             "Cannot allocate replacement; text unchanged");
    return 0;
  }
  if (changed) e->view->doc->dirty = 1;
  s->replaced++;
  selection_clear(e);
  dispatch_change(e);
  if (s->workspace) {
    snprintf(e->notice, sizeof e->notice,
             "Replaced in buffer; save before searching disk again");
    close_search(e, 1);
  } else {
    s->oy = h.row;
    s->ox = h.col + (int)strlen(s->replacement);
    update(e);
    s->phase = ACTIONS;
  }
  return 1;
}
static void workspace_replace_confirmed(editor *e) {
  search_state *s = e->search;
  if (documents_unsaved_in(e, e->files.workspace_root) ||
      e->git.action != GIT_IDLE) {
    snprintf(e->notice, sizeof e->notice,
             "Save workspace buffers and finish Git operations first");
    return;
  }
  search_job *next = search_job_replace(s->job, s->replacement);
  if (!next) {
    snprintf(e->notice, sizeof e->notice, "Cannot start replacement");
    return;
  }
  stop(s);
  s->job = next;
  if (!s->job) {
    snprintf(e->notice, sizeof e->notice, "Cannot start replacement");
    return;
  }
  s->phase = WORKSPACE_REPLACING;
  s->cancelled = 0;
}
static void replace_all(editor *e) {
  search_state *s = e->search;
  search_job_progress(s->job, &s->progress);
  if (!s->progress.done) {
    snprintf(e->notice, sizeof e->notice,
             "Search still running; wait for the final count");
    return;
  }
  if (s->progress.failed || !s->progress.matches)
    return;
  if (s->workspace) {
    if (documents_unsaved_in(e, e->files.workspace_root) ||
        e->git.action != GIT_IDLE) {
      snprintf(e->notice, sizeof e->notice,
               "Save workspace buffers and finish Git operations first");
      return;
    }
    snprintf(s->confirmation, sizeof s->confirmation,
             "Replace %llu matches in %zu workspace files on disk?",
             (unsigned long long)s->progress.matches, s->progress.files);
    dispatch_confirm(e, workspace_replace_confirmed, s->confirmation);
  } else if (documents_editable(e)) {
    search_job_cancel(s->job, 1);
    undo_group_begin(e);
    s->replace_index = 0;
    s->phase = FILE_REPLACING;
    s->cancelled = 0;
  }
}
static int cached_hit(search_state *s, uint64_t index, search_hit *hit) {
  if (!s->cache_count || index < s->cache_first ||
      index >= s->cache_first + s->cache_count) {
    s->cache_first = index / 256 * 256;
    s->cache_count = search_job_hits(s->job, s->cache_first, s->cache, 256);
  }
  if (index < s->cache_first || index >= s->cache_first + s->cache_count)
    return 0;
  *hit = s->cache[index - s->cache_first];
  return 1;
}
// Construct each changed line once, using the disk index. No character-by-
// character reallocations or quadratic shifting for replace-all.
static void replace_step(editor *e) {
  search_state *s = e->search;
  if (s->replace_index >= s->progress.matches) {
    undo_group_end(e);
    s->phase = ACTIONS;
    s->oy = e->view->cy;
    s->ox = e->view->cx;
    update(e);
    return;
  }
  search_hit first;
  if (!cached_hit(s, s->replace_index, &first)) {
    close_search(e, 1);
    return;
  }
  const char *line = buffer_line(&e->view->doc->buf, first.row);
  if (!line) {
    close_search(e, 1);
    return;
  }
  size_t old = strlen(line), size = old, rlen = strlen(s->replacement);
  uint64_t end = s->replace_index;
  search_hit h;
  while (end < s->progress.matches && cached_hit(s, end, &h) &&
         h.row == first.row) {
    if (rlen > (size_t)INT_MAX - 1 - (size - h.length)) {
      snprintf(e->notice, sizeof e->notice,
               "Replacement would exceed line size limit");
      close_search(e, 1);
      return;
    }
    size = size - h.length + rlen;
    end++;
  }
  char *text = malloc(size + 1);
  if (!text) {
    snprintf(e->notice, sizeof e->notice,
             "Out of memory; remaining matches unchanged");
    close_search(e, 1);
    return;
  }
  size_t write = 0, read = 0;
  for (uint64_t i = s->replace_index; i < end; i++) {
    if (!cached_hit(s, i, &h)) {
      free(text);
      close_search(e, 1);
      return;
    }
    size_t n = (size_t)h.col - read;
    memcpy(text + write, line + read, n);
    write += n;
    memcpy(text + write, s->replacement, rlen);
    write += rlen;
    read = (size_t)h.col + h.length;
  }
  memcpy(text + write, line + read, old - read + 1);
  if (!strcmp(text, line)) {
    free(text);
    s->replaced += end - s->replace_index;
    s->replace_index = end;
    return;
  }
  // Apply individual edits from right to left so every other shared cursor
  // retains its position relative to unchanged text, then install once.
  if (buffer_replace_line(&e->view->doc->buf, first.row, text) < 0) {
    free(text);
    snprintf(e->notice, sizeof e->notice, "Cannot replace line");
    close_search(e, 1);
    return;
  }
  buffer *b = &e->view->doc->buf;
  for (uint64_t i = end; i > s->replace_index;) {
    if (!cached_hit(s, --i, &h))
      break;
    buffer_edit edit = {
        h.row, h.col, h.row, h.col + h.length, h.row, h.col + (int)rlen};
    if (b->on_edit)
      b->on_edit(b, &edit, b->edit_context);
    view_rebase(e->view, &edit);
  }
  s->replaced += end - s->replace_index;
  s->replace_index = end;
  e->view->doc->dirty = 1;
  selection_clear(e);
  dispatch_change(e);
}
void search_tick(editor *e) {
  search_state *s = e->search;
  if (!s || !s->active || s->goto_mode || !s->job)
    return;
  if (s->phase == FILE_REPLACING) {
    replace_step(e);
    return;
  }
  search_job_progress(s->job, &s->progress);
  if (s->phase == WORKSPACE_REPLACING) {
    if (s->progress.done) {
      if (s->progress.changed)
        documents_reload(e, e->files.workspace_root);
      snprintf(e->notice, sizeof e->notice,
               "%s: %llu files changed, %llu skipped%s%s",
               s->cancelled ? "Replacement cancelled" : "Replacement finished",
               (unsigned long long)s->progress.changed,
               (unsigned long long)s->progress.skipped,
               s->progress.failed ? "; " : "",
               s->progress.failed ? s->progress.error : "");
      close_search(e, 1);
    }
    return;
  }
  if (s->selected >= s->progress.matches)
    s->selected = 0;
  if (s->selected_file >= s->progress.files)
    s->selected_file = 0;
  if (!s->workspace && s->initial && s->progress.matches) {
    uint64_t lo = 0, hi = s->progress.matches;
    search_hit h;
    while (lo < hi) {
      uint64_t mid = lo + (hi - lo) / 2;
      if (!search_job_hit(s->job, mid, &h, NULL))
        break;
      if (h.row < s->oy || (h.row == s->oy && h.col < s->ox))
        lo = mid + 1;
      else
        hi = mid;
    }
    if (lo < s->progress.matches || s->progress.done) {
      s->selected = lo < s->progress.matches ? lo : 0;
      s->initial = 0;
      show_hit(e);
    }
  }
}
static char *field(search_state *s, int *length) {
  if (s->phase == REPLACEMENT) {
    *length = s->replacement_length;
    return s->replacement;
  }
  *length = s->length;
  return s->query;
}
static void input(editor *e, int key) {
  search_state *s = e->search;
  int length;
  char *text = field(s, &length);
  if (key == KEY_BACKSPACE || key == CTRL('h')) {
    if (s->input_cursor) {
      int start = grapheme_prev(text, s->input_cursor);
      memmove(text + start, text + s->input_cursor,
              (size_t)(length - s->input_cursor + 1));
      length -= s->input_cursor - start;
      s->input_cursor = start;
    }
  } else if (key == KEY_DELETE) {
    if (s->input_cursor < length) {
      int end = grapheme_next(text, s->input_cursor);
      memmove(text + s->input_cursor, text + end, (size_t)(length - end + 1));
      length -= end - s->input_cursor;
    }
  } else if (key == KEY_LEFT) {
    s->input_cursor = grapheme_prev(text, s->input_cursor);
    return;
  } else if (key == KEY_RIGHT) {
    if (s->input_cursor < length)
      s->input_cursor = grapheme_next(text, s->input_cursor);
    return;
  } else if (key == KEY_HOME || key == CTRL('a')) {
    s->input_cursor = 0;
    return;
  } else if (key == KEY_END || key == CTRL('e')) {
    s->input_cursor = length;
    return;
  } else if (key >= 32 && key < KEY_SPECIAL && key != KEY_BACKSPACE) {
    if (s->goto_mode && (key < '0' || key > '9' || length >= 9))
      return;
    char bytes[4];
    int n = utf8_encode(key, bytes);
    if (!n || length + n >= PATTERN_MAX)
      return;
    memmove(text + s->input_cursor + n, text + s->input_cursor,
            (size_t)(length - s->input_cursor + 1));
    memcpy(text + s->input_cursor, bytes, n);
    length += n;
    s->input_cursor += n;
  } else
    return;
  if (s->phase == REPLACEMENT)
    s->replacement_length = length;
  else {
    s->length = length;
    update(e);
  }
}
static int on_key(editor *e, int key) {
  search_state *s = e->search;
  if (!s || !s->active)
    return 0;
  search_tick(e);
  if (!s->active)
    return 0;
  if (s->phase == WORKSPACE_REPLACING) {
    if (key == '\x1b' || key == CTRL('g')) {
      s->cancelled = 1;
      search_job_cancel(s->job, 0);
    }
    return 1;
  }
  if (s->phase == FILE_REPLACING) {
    if (key == '\x1b' || key == CTRL('g'))
      close_search(e, 1);
    return 1;
  }
  if (key == '\x1b' || key == CTRL('g')) {
    close_search(e, 0);
    return 1;
  }
  if (s->goto_mode) {
    if (key == '\r' || key == '\n')
      close_search(e, s->length > 0);
    else
      input(e, key);
    return 1;
  }
  if (s->phase == QUERY && key == '\t') {
    s->regex = !s->regex;
    update(e);
    return 1;
  }
  if (s->phase == QUERY && (key == KEY_UP || key == CTRL('r') ||
                            key == KEY_DOWN || key == CTRL('s'))) {
    move_hit(e, key == KEY_UP || key == CTRL('r') ? -1 : 1, 0);
    return 1;
  }
  if (key == '\r' || key == '\n') {
    if (s->phase == QUERY) {
      if (!s->valid || !s->length)
        return 1;
      if (s->replacing) {
        s->phase = REPLACEMENT;
        s->input_cursor = s->replacement_length;
      } else if (s->workspace) {
        s->phase = RESULTS;
        show_hit(e);
      } else
        close_search(e, 1);
    } else if (s->phase == REPLACEMENT) {
      s->phase = ACTIONS;
      if (!s->workspace)
        show_hit(e);
    } else
      show_hit(e);
    return 1;
  }
  if (s->phase == RESULTS || s->phase == ACTIONS) {
    if (s->workspace && key == CTRL('b')) {
      e->sidebar.focused = 1;
      return 1;
    }
    if (s->workspace && key == CTRL('l')) {
      e->sidebar.focused = 0;
      return 1;
    }
    if (s->workspace && e->sidebar.focused &&
        (key == KEY_UP || key == KEY_DOWN || key == KEY_LEFT ||
         key == KEY_RIGHT || key == KEY_PGUP || key == KEY_PGDOWN)) {
      search_files_key(e, key);
      return 1;
    }
    if (key == KEY_UP || key == CTRL('r'))
      move_hit(e, -1, 1);
    else if (key == KEY_DOWN || key == CTRL('s') || key == 'n')
      move_hit(e, 1, 1);
    else if (s->phase == ACTIONS && key == 'y')
      replace_one(e);
    else if (s->phase == ACTIONS && key == 'a')
      replace_all(e);
    return 1;
  }
  input(e, key);
  return 1;
}
int search_active(const editor *e) { return e->search && e->search->active; }
static void bar(editor *e, char *text, size_t size, int *cursor) {
  search_state *s = e->search;
  *cursor = -1;
  if (s->goto_mode) {
    snprintf(text, size, " GOTO %s", s->query);
    *cursor = 6 + s->input_cursor;
    return;
  }
  const char *scope = s->workspace ? "WORKSPACE" : "FILE",
             *mode = s->regex ? "REGEX" : "TEXT";
  const char *running = s->progress.done ? "" : "...";
  if (s->phase == WORKSPACE_REPLACING) {
    snprintf(text, size, " REPLACE %llu matches / %llu files%s  Esc: cancel",
             (unsigned long long)s->progress.matches,
             (unsigned long long)s->progress.changed, running);
    return;
  }
  if (s->phase == FILE_REPLACING) {
    snprintf(text, size, " REPLACE %llu/%llu  Esc: stop",
             (unsigned long long)s->replace_index,
             (unsigned long long)s->progress.matches);
    return;
  }
  int length;
  char *q = field(s, &length);
  char suffix[180];
  if (s->phase == REPLACEMENT)
    snprintf(suffix, sizeof suffix, "  Enter: actions  Esc: cancel");
  else if (e->notice[0])
    snprintf(suffix, sizeof suffix, "  %.100s", e->notice);
  else if (!s->valid)
    snprintf(suffix, sizeof suffix, "  invalid regex");
  else if (s->progress.failed)
    snprintf(suffix, sizeof suffix, "  %.70s", s->progress.error);
  else
    snprintf(suffix, sizeof suffix, "  %llu/%llu%s  %s%s",
             (unsigned long long)(s->progress.matches ? s->selected + 1 : 0),
             (unsigned long long)s->progress.matches, running,
             s->phase == ACTIONS   ? "y:replace n:next a:all"
             : s->phase == RESULTS ? "Up/Down: hits Enter:open"
                                   : "Up/Down: hits Tab:regex",
             s->progress.skipped ? " [skipped]" : "");
  char prefix[60];
  snprintf(prefix, sizeof prefix, " %s %s %s ",
           s->phase == REPLACEMENT ? "REPLACE" : "SEARCH", scope, mode);
  if (e->cols < 60)
    snprintf(prefix, sizeof prefix, " %s %s ",
             s->phase == REPLACEMENT ? "REPL"
             : s->workspace          ? "WS"
                                     : "FIND",
             s->regex ? "RX" : "TXT");
  if (e->cols < 60 && s->phase != REPLACEMENT && s->valid &&
      !s->progress.failed)
    snprintf(suffix, sizeof suffix, " %llu/%llu%s %s",
             (unsigned long long)(s->progress.matches ? s->selected + 1 : 0),
             (unsigned long long)s->progress.matches, running,
             s->phase == ACTIONS   ? "y n a"
             : s->phase == RESULTS ? "Up/Dn Enter"
                                   : "Up/Dn Tab:RX");
  int reserve = (int)strlen(prefix) + (int)strlen(suffix),
      avail = e->cols - reserve;
  if (avail < 4 && s->valid && !s->progress.failed && !e->notice[0]) {
    snprintf(suffix, sizeof suffix, " %llu/%llu%s",
             (unsigned long long)(s->progress.matches ? s->selected + 1 : 0),
             (unsigned long long)s->progress.matches, running);
    avail = e->cols - (int)strlen(prefix) - (int)strlen(suffix);
  }
  if (avail < 0)
    avail = 0;
  int start = 0;
  while (start < s->input_cursor &&
         utf8_cols(q + start, s->input_cursor - start) > avail)
    start = grapheme_next(q, start);
  int end = start;
  while (end < length &&
         utf8_cols(q + start, grapheme_next(q, end) - start) <= avail)
    end = grapheme_next(q, end);
  snprintf(text, size, "%s%.*s%s", prefix, end - start, q + start, suffix);
  if (s->phase == QUERY || s->phase == REPLACEMENT)
    *cursor =
        (int)strlen(prefix) + utf8_cols(q + start, s->input_cursor - start);
}
static void on_draw(editor *e, abuf *ab) {
  if (!search_active(e) || e->confirmation)
    return;
  search_tick(e);
  if (!search_active(e))
    return;
  char text[600];
  int cursor;
  bar(e, text, sizeof text, &cursor);
  screen_position(ab, 0, e->rows - 1);
  theme_append(ab, THEME_STATUS);
  int used = screen_text(ab, text, e->cols);
  screen_repeat(ab, ' ', e->cols - used);
  theme_append(ab, THEME_NORMAL);
}
int search_cursor(const editor *e, int *x, int *y) {
  if (!search_active(e) || e->confirmation)
    return 0;
  char text[600];
  int cursor;
  bar((editor *)e, text, sizeof text, &cursor);
  if (cursor < 0 || e->cols <= 0 || e->rows <= 0)
    return 0;
  *x = cursor < e->cols ? cursor : e->cols - 1;
  *y = e->rows - 1;
  return 1;
}
int search_files_draw(editor *e, abuf *ab, rect area) {
  search_state *s = e->search;
  if (!s || !s->active || !s->workspace)
    return 0;
  search_job_progress(s->job, &s->progress);
  screen_fill(ab, area, ' ');
  screen_position(ab, area.x, area.y);
  const char *focus = e->sidebar.focused ? theme_colour(THEME_ACTIVE) : theme_colour(THEME_INACTIVE);
  ab_append(ab, focus, (int)strlen(focus));
  int used = screen_text(
      ab, e->sidebar.focused ? " FILES * SEARCH" : " FILES SEARCH", area.width);
  screen_repeat(ab, ' ', area.width - used);
  theme_append(ab, THEME_NORMAL);
  int rows = (area.height - 2) / 2;
  if (rows < 1)
    rows = 1;
  if (s->selected_file < s->file_offset)
    s->file_offset = s->selected_file;
  if (s->selected_file >= s->file_offset + (size_t)rows)
    s->file_offset = s->selected_file - rows + 1;
  for (int i = 0; i < rows && area.y + i * 2 + 2 < area.y + area.height - 1;
       i++) {
    size_t index = s->file_offset + (size_t)i;
    search_file file;
    if (!search_job_file(s->job, index, &file))
      break;
    if (index == s->selected_file)
      ab_append(ab, focus, (int)strlen(focus));
    screen_position(ab, area.x, area.y + i * 2 + 1);
    char label[400];
    snprintf(label, sizeof label, "%s [%llu]", path_name(file.path),
             (unsigned long long)file.count);
    used = screen_text(ab, label, area.width);
    screen_repeat(ab, ' ', area.width - used);
    screen_position(ab, area.x, area.y + i * 2 + 2);
    int start = 0;
    for (int col = 0; file.path[start] && col < s->path_offset; col++)
      start = grapheme_next(file.path, start);
    used = screen_text(ab, file.path + start, area.width);
    screen_repeat(ab, ' ', area.width - used);
    theme_append(ab, THEME_NORMAL);
    free(file.path);
  }
  screen_position(ab, area.x, area.y + area.height - 1);
  screen_text(ab, "Left/Right: path  Enter:open", area.width);
  screen_fill(ab, (rect){area.width, area.y, 1, area.height}, '|');
  return 1;
}
int search_files_key(editor *e, int key) {
  search_state *s = e->search;
  if (!s || !s->active || !s->workspace)
    return 0;
  search_job_progress(s->job, &s->progress);
  int step = key == KEY_UP       ? -1
             : key == KEY_DOWN   ? 1
             : key == KEY_PGUP   ? -5
             : key == KEY_PGDOWN ? 5
                                 : 0;
  if (step && s->progress.files) {
    if (step < 0)
      s->selected_file =
          (size_t)(-step) > s->selected_file ? 0 : s->selected_file + step;
    else
      s->selected_file = s->selected_file + (size_t)step < s->progress.files
                             ? s->selected_file + step
                             : s->progress.files - 1;
    search_file f;
    if (search_job_file(s->job, s->selected_file, &f)) {
      s->selected = f.first;
      free(f.path);
    }
  } else if (key == KEY_LEFT) {
    if (s->path_offset)
      s->path_offset--;
  } else if (key == KEY_RIGHT) {
    if (s->path_offset < INT_MAX)
      s->path_offset++;
  } else if (key == '\r' || key == '\n')
    show_hit(e);
  return 1;
}
void search_status(editor *e, unsigned long long *matches, int *done) {
  search_tick(e);
  search_state *s = e->search;
  *matches = s ? (unsigned long long)s->progress.matches : 0;
  *done = !s || s->progress.done;
}
static void init(editor *e) {
  e->search = calloc(1, sizeof *e->search);
  dispatch_bind(CTRL('s'), search_begin);
  dispatch_bind_prefix('s', search_begin, "s", "Search current file");
  dispatch_bind_prefix_global('S', workspace_begin, "S", "Search workspace");
  dispatch_bind_prefix('r', replace_begin, "r", "Search and replace in file");
  dispatch_bind_prefix_global('R', workspace_replace_begin, "R",
                              "Search and replace in workspace");
  dispatch_bind_prefix('l', goto_begin, "l", "Go to Line");
}
static void search_shutdown(editor *e) {
  if (e->search) {
    if (e->search->phase == FILE_REPLACING) undo_group_end(e);
    stop(e->search);
    free(e->search);
    e->search = NULL;
  }
}
static module search = {.name = "search",
                        .init = init,
                        .on_key = on_key,
                        .on_draw = on_draw,
                        .shutdown = search_shutdown};
module *search_module(void) { return &search; }
