#include "undo.h"
#include "cursor.h"
#include "dispatch.h"
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#define seek _fseeki64
#define tell _ftelli64
#else
#define seek fseeko
#define tell ftello
#endif
#define HISTORY_ACTIONS 1000
#define COMPACT_INTERVAL (8u * 1024u * 1024u)

typedef struct undo_edit {
  struct undo_edit *previous, *next;
  buffer_edit span;
  uint64_t old_offset, new_offset, old_bytes, new_bytes, hints_offset, hints;
  int before_empty, after_empty, external;
} undo_edit;
typedef struct undo_group {
  struct undo_group *previous, *next;
  undo_edit *first, *last;
  size_t count;
  uint64_t before_state, after_state;
  view before, after;
  int kind;
  double when;
} undo_group;
struct undo_history {
  FILE *journal;
  uint64_t offset, state, serial, saved, compact_at;
  undo_group *oldest, *undo, *redo, *pending;
  size_t actions;
  int replaying;
};
struct undo_state {
  view before;
  document *active;
  int key, depth, hold, kind;
};
static double now(void) {
#ifdef _WIN32
  return (double)GetTickCount64() / 1000.0;
#else
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t))
    timespec_get(&t, TIME_UTC);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
#endif
}
static void free_group(undo_group *g) {
  if (!g)
    return;
  undo_edit *r = g->first;
  while (r) {
    undo_edit *next = r->next;
    free(r);
    r = next;
  }
  free(g);
}
void undo_clear(document *doc) {
  if (!doc || !doc->history)
    return;
  struct undo_history *h = doc->history;
  editor *e = doc->buf.edit_context;
  if (e && e->undo && e->undo->active == doc) {
    e->undo->active = NULL;
    e->undo->hold = 0;
  }
  undo_group *g = h->oldest;
  while (g) {
    undo_group *next = g->next;
    free_group(g);
    g = next;
  }
  g = h->redo;
  while (g) {
    undo_group *next = g->next;
    free_group(g);
    g = next;
  }
  free_group(h->pending);
  if (h->journal)
    fclose(h->journal);
  free(h);
  doc->history = NULL;
}
static void discard_redo(struct undo_history *h) {
  undo_group *g = h->redo;
  while (g) {
    undo_group *next = g->next;
    free_group(g);
    g = next;
  }
  h->redo = NULL;
}
static FILE *journal_open(void) {
#ifdef _WIN32
  // CRT tmpfile implementations can choose the drive root. Use the user's
  // temporary directory and let Windows unlink the journal when it closes.
  wchar_t directory[MAX_PATH + 1], name[MAX_PATH + 1];
  DWORD n = GetTempPathW(MAX_PATH + 1, directory);
  if (!n || n > MAX_PATH || !GetTempFileNameW(directory, L"adm", 0, name))
    return NULL;
  HANDLE handle =
      CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                  FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
  if (handle == INVALID_HANDLE_VALUE) {
    DeleteFileW(name);
    return NULL;
  }
  int fd = _open_osfhandle((intptr_t)handle, _O_BINARY | _O_RDWR);
  if (fd < 0) {
    CloseHandle(handle);
    return NULL;
  }
  FILE *file = _fdopen(fd, "w+b");
  if (!file)
    _close(fd);
  return file;
#else
  return tmpfile();
#endif
}
static struct undo_history *history(document *doc) {
  if (doc->history)
    return doc->history;
  struct undo_history *h = calloc(1, sizeof *h);
  if (!h)
    return NULL;
  h->journal = journal_open();
  if (!h->journal) {
    free(h);
    return NULL;
  }
  h->saved = doc->dirty ? UINT64_MAX : 0;
  doc->history = h;
  return h;
}
static int write_bytes(struct undo_history *h, const char *s, size_t n) {
#ifdef ADM_TEST_ALLOC
  extern int undo_io_failed;
  if (undo_io_failed)
    return 0;
#endif
  if (n > INT64_MAX || h->offset > INT64_MAX - n)
    return 0;
  if (tell(h->journal) != (int64_t)h->offset &&
      seek(h->journal, (int64_t)h->offset, SEEK_SET))
    return 0;
  if (n && fwrite(s, 1, n, h->journal) != n)
    return 0;
  h->offset += n;
  return 1;
}
static int removed_text(buffer *b, const buffer_edit *edit,
                        struct undo_history *h, uint64_t *bytes) {
  *bytes = 0;
  if (edit->row == edit->old_row && edit->col == edit->old_col)
    return 1;
  for (int row = edit->row; row <= edit->old_row; row++) {
    const char *line = buffer_line(b, row);
    if (!line)
      return 0;
    size_t start = row == edit->row ? (size_t)edit->col : 0;
    size_t end = row == edit->old_row ? (size_t)edit->old_col : strlen(line);
    if (end < start || end > strlen(line) ||
        !write_bytes(h, line + start, end - start))
      return 0;
    *bytes += end - start;
    if (row != edit->old_row) {
      if (!write_bytes(h, "\n", 1))
        return 0;
      (*bytes)++;
    }
  }
  return 1;
}
static void commit(editor *e);
static int capture(buffer *b, const buffer_edit *span, const char *text,
                   size_t bytes, int before, int after, void *context) {
  document *doc = context;
  struct undo_history *h = doc->history;
  if (h && h->replaying)
    return 1;
  editor *e = b->edit_context;
  if (!e || doc->readonly)
    return 0;
  if (!e->undo)
    goto error;
  if (!bytes && span->row == span->old_row && span->col == span->old_col &&
      before == after)
    return 1;
  if (!h && !(h = history(doc)))
    goto error;
  struct undo_state *s = e->undo;
  if (s->active && s->active != doc) {
    int hold = s->hold;
    s->hold = 0;
    commit(e);
    s->hold = hold;
  }
  undo_group *g = h->pending;
  int created = !g;
  if (!g) {
    g = calloc(1, sizeof *g);
    if (!g)
      goto error;
    g->before =
        s->before.doc == doc
            ? s->before
            : (view){.doc = doc, .used = 1, .cx = span->col, .cy = span->row};
    g->before_state = h->state;
    g->kind = s->depth ? s->kind : 0;
  }
  uint64_t offset = h->offset;
  undo_edit *last = g->last;
  // Adjacent insertions, including UTF-8 bytes, share a single inverse range.
  int extend = last && !last->external && !b->external_edits &&
               !last->old_bytes && span->row == span->old_row &&
               span->col == span->old_col && span->row == last->span.new_row &&
               span->col == last->span.new_col &&
               offset == last->new_offset + last->new_bytes;
  int extend_delete =
      last && !last->external && !b->external_edits && !last->new_bytes &&
      !bytes && last->span.row == last->span.old_row &&
      span->row == span->old_row && span->row == last->span.row &&
      span->col == last->span.col &&
      offset == last->old_offset + last->old_bytes;
  undo_edit *r = extend || extend_delete ? last : calloc(1, sizeof *r);
  if (!r) {
    if (created)
      free_group(g);
    goto error;
  }
  uint64_t removed = 0;
  if ((!extend && !removed_text(b, span, h, &removed)) ||
      (!write_bytes(h, text, bytes) || fflush(h->journal))) {
    h->offset = offset;
    clearerr(h->journal);
    if (!extend && !extend_delete)
      free(r);
    if (created)
      free_group(g);
    goto error;
  }
  if (extend) {
    r->new_bytes += bytes;
    r->span.new_row = span->new_row;
    r->span.new_col = span->new_col;
    r->after_empty = !after;
  } else if (extend_delete) {
    r->old_bytes += removed;
    r->span.old_col += span->old_col - span->col;
    r->after_empty = !after;
  } else {
    *r = (undo_edit){.previous = last,
                     .span = *span,
                     .old_offset = offset,
                     .new_offset = offset + removed,
                     .old_bytes = removed,
                     .new_bytes = bytes,
                     .before_empty = !before,
                     .after_empty = !after,
                     .external = b->external_edits};
    if (last)
      last->next = r;
    else
      g->first = r;
    g->last = r;
    g->count++;
  }
  h->pending = g;
  s->active = doc;
  return 1;
error:
  snprintf(
      e->notice, sizeof e->notice,
      "Cannot record undo; edit cancelled (memory or temporary disk full)");
  return 0;
}
void undo_attach(document *doc) {
  if (!doc)
    return;
  doc->buf.before_change = capture;
  doc->buf.change_context = doc;
}
void undo_note_edit(buffer *b, const buffer_edit *edit) {
  document *doc = b->change_context;
  if (!doc || !doc->history)
    return;
  struct undo_history *h = doc->history;
  undo_group *g = h->pending;
  if (h->replaying || !g || !g->last->external)
    return;
  undo_edit *r = g->last;
  if (!r->hints)
    r->hints_offset = h->offset;
  if (write_bytes(h, (const char *)edit, sizeof *edit))
    r->hints++;
  else {
    // The text inverse is already complete. Fall back to its enclosing span.
    r->external = 0;
    r->hints = 0;
    clearerr(h->journal);
  }
}
// Reclaim payloads from evicted actions and abandoned redo branches. Offsets
// change only after the replacement journal has been written successfully.
static void compact(struct undo_history *h) {
  if (h->offset < COMPACT_INTERVAL || h->offset < h->compact_at)
    return;
  h->compact_at = h->offset <= INT64_MAX - COMPACT_INTERVAL
                      ? h->offset + COMPACT_INTERVAL
                      : UINT64_MAX;
  uint64_t live = 0;
  size_t count = 0;
  for (undo_group *g = h->oldest; g; g = g->next)
    for (undo_edit *r = g->first; r; r = r->next) {
      live += r->old_bytes + r->new_bytes + r->hints * sizeof(buffer_edit);
      count++;
    }
  if (live > h->offset / 2 || h->offset - live < COMPACT_INTERVAL ||
      count > SIZE_MAX / (3 * sizeof(uint64_t)))
    return;
  uint64_t *offsets = malloc(count * 3 * sizeof *offsets);
  FILE *next = journal_open();
  if (!offsets || !next) {
    free(offsets);
    if (next)
      fclose(next);
    return;
  }
  uint64_t written = 0;
  size_t index = 0;
  char chunk[65536];
  for (undo_group *g = h->oldest; g; g = g->next)
    for (undo_edit *r = g->first; r; r = r->next) {
      uint64_t starts[] = {r->old_offset, r->new_offset, r->hints_offset};
      uint64_t lengths[] = {r->old_bytes, r->new_bytes,
                            r->hints * sizeof(buffer_edit)};
      for (int i = 0; i < 3; i++) {
        offsets[index++] = written;
        if (lengths[i] && seek(h->journal, (int64_t)starts[i], SEEK_SET))
          goto fail;
        while (lengths[i]) {
          size_t n =
              lengths[i] < sizeof chunk ? (size_t)lengths[i] : sizeof chunk;
          if (fread(chunk, 1, n, h->journal) != n ||
              fwrite(chunk, 1, n, next) != n)
            goto fail;
          lengths[i] -= n;
          written += n;
        }
      }
    }
  if (fflush(next))
    goto fail;
  index = 0;
  for (undo_group *g = h->oldest; g; g = g->next)
    for (undo_edit *r = g->first; r; r = r->next) {
      r->old_offset = offsets[index++];
      r->new_offset = offsets[index++];
      r->hints_offset = offsets[index++];
    }
  fclose(h->journal);
  h->journal = next;
  h->offset = written;
  h->compact_at = written + COMPACT_INTERVAL;
  free(offsets);
  return;
fail:
  clearerr(h->journal);
  fclose(next);
  free(offsets);
}
static void commit(editor *e) {
  struct undo_state *s = e->undo;
  if (!s || !s->active || s->hold)
    return;
  document *doc = s->active;
  struct undo_history *h = doc->history;
  undo_group *g = h ? h->pending : NULL;
  s->active = NULL;
  if (!g)
    return;
  h->pending = NULL;
  g->after = e->view->doc == doc ? *e->view : g->before;
  g->after_state = ++h->serial;
  g->when = now();
  discard_redo(h);
  undo_group *last = h->undo;
  // Navigation, selections, other commands, saves and switching views stop a
  // run.
  if (last && g->kind && last->kind == g->kind &&
      (g->kind == 1 || last->count + g->count <= 128) && h->saved != h->state &&
      g->when - last->when <= 1.0 && g->before.doc == last->after.doc &&
      g->before.revision == last->after.revision &&
      g->before.cx == last->after.cx && g->before.cy == last->after.cy &&
      !g->before.sel_active && !last->after.sel_active) {
    undo_edit *a = last->last, *b = g->first;
    int inserts = !a->old_bytes && !b->old_bytes &&
                  a->span.new_row == b->span.row &&
                  a->span.new_col == b->span.col &&
                  a->new_offset + a->new_bytes == b->new_offset;
    int deletes = !a->new_bytes && !b->new_bytes &&
                  a->span.row == a->span.old_row &&
                  b->span.row == b->span.old_row &&
                  a->span.row == b->span.row && a->span.col == b->span.col &&
                  a->old_offset + a->old_bytes == b->old_offset;
    if (!a->external && !b->external && (inserts || deletes)) {
      if (inserts) {
        a->new_bytes += b->new_bytes;
        a->span.new_row = b->span.new_row;
        a->span.new_col = b->span.new_col;
      } else {
        a->old_bytes += b->old_bytes;
        a->span.old_col += b->span.old_col - b->span.col;
      }
      a->after_empty = b->after_empty;
      a->next = b->next;
      if (b->next)
        b->next->previous = a;
      last->last = g->last == b ? a : g->last;
      last->count += g->count - 1;
      free(b);
    } else {
      a->next = b;
      b->previous = a;
      last->last = g->last;
      last->count += g->count;
    }
    last->after = g->after;
    last->after_state = g->after_state;
    last->when = g->when;
    free(g);
  } else {
    g->previous = last;
    if (last)
      last->next = g;
    else
      h->oldest = g;
    h->undo = g;
    h->actions++;
  }
  h->state = h->undo->after_state;
  doc->dirty = h->state != h->saved;
  while (h->actions > HISTORY_ACTIONS) {
    undo_group *old = h->oldest;
    h->oldest = old->next;
    h->oldest->previous = NULL;
    free_group(old);
    h->actions--;
  }
  compact(h);
}
struct undo_transaction {
  document *doc;
  undo_group *group;
  uint64_t journal_before, journal_after;
};
struct undo_transaction *undo_prepare(editor *e, document *doc,
                                      const buffer_edit *edit, const char *text,
                                      size_t bytes, int after_lines,
                                      const buffer_edit *events,
                                      size_t event_count) {
  struct undo_history *h = history(doc);
  if (!h || h->pending || !e->undo || e->undo->hold)
    return NULL;
  struct undo_transaction *t = calloc(1, sizeof *t);
  undo_group *g = calloc(1, sizeof *g);
  undo_edit *r = calloc(1, sizeof *r);
  if (!t || !g || !r) {
    free(t);
    free(g);
    free(r);
    return NULL;
  }
  uint64_t offset = h->offset, removed = 0;
  if (!removed_text(&doc->buf, edit, h, &removed) ||
      !write_bytes(h, text, bytes) ||
      (event_count &&
       !write_bytes(h, (const char *)events, event_count * sizeof *events)) ||
      fflush(h->journal)) {
    h->offset = offset;
    clearerr(h->journal);
    free(t);
    free(g);
    free(r);
    return NULL;
  }
  g->before = (view){.doc = doc, .used = 1, .cy = edit->row, .cx = edit->col};
  for (editor_tab *tab = e->tabs; tab; tab = tab->next) {
    workspace *w = tabs_workspace(e, tab);
    for (int i = 0; i < MAX_PANES; i++)
      if (w->panes[i].doc == doc) {
        g->before = w->panes[i];
        break;
      }
  }
  if (e->view->doc == doc)
    g->before = *e->view;
  *r = (undo_edit){.span = *edit,
                   .old_offset = offset,
                   .new_offset = offset + removed,
                   .old_bytes = removed,
                   .new_bytes = bytes,
                   .before_empty = !doc->buf.nlines,
                   .after_empty = !after_lines,
                   .external = event_count != 0,
                   .hints = event_count,
                   .hints_offset = offset + removed + bytes};
  g->first = g->last = r;
  g->count = 1;
  g->before_state = h->state;
  t->doc = doc;
  t->group = g;
  t->journal_before = offset;
  t->journal_after = h->offset;
  return t;
}
void undo_discard_prepared(struct undo_transaction *t) {
  if (t) {
    if (t->doc->history && t->doc->history->offset == t->journal_after)
      t->doc->history->offset = t->journal_before;
    free_group(t->group);
    free(t);
  }
}
void undo_commit_prepared(editor *e, struct undo_transaction *t) {
  struct undo_history *h = t->doc->history;
  h->pending = t->group;
  e->undo->active = t->doc;
  commit(e);
  free(t);
}
void undo_saved(document *doc) {
  if (doc && doc->history)
    doc->history->saved = doc->history->state;
}
void undo_begin(editor *e, int key) {
  if (!e->undo)
    return;
  struct undo_state *s = e->undo;
  if (!s->depth && !s->hold) {
    commit(e);
    s->before = *e->view;
    s->key = key;
    s->kind = key == KEY_BACKSPACE                                     ? 2
              : key == KEY_DELETE || key == CTRL('d')                  ? 3
              : key >= 128 && key < KEY_SPECIAL                        ? 1
              : key >= 32 && key < 127 && (isalnum(key) || key == '_') ? 1
                                                                       : 0;
    // A non-edit key is a grouping boundary, including a move away and back.
    document *doc = e->view->doc;
    if (!s->kind && doc && doc->history && doc->history->undo)
      doc->history->undo->kind = 0;
  }
  s->depth++;
}
void undo_end(editor *e) {
  if (!e->undo)
    return;
  if (e->undo->depth)
    e->undo->depth--;
  if (!e->undo->depth)
    commit(e);
}
void undo_group_begin(editor *e) {
  if (!e->undo)
    return;
  if (!e->undo->hold) {
    commit(e);
    e->undo->before = *e->view;
    e->undo->kind = 0;
  }
  e->undo->hold++;
}
void undo_group_end(editor *e) {
  if (!e->undo)
    return;
  if (e->undo->hold)
    e->undo->hold--;
  commit(e);
}
static char *read_text(struct undo_history *h, uint64_t offset,
                       uint64_t bytes) {
#ifdef ADM_TEST_ALLOC
  extern int undo_io_failed;
  if (undo_io_failed)
    return NULL;
#endif
  if (bytes >= SIZE_MAX || offset > INT64_MAX)
    return NULL;
  char *text = malloc((size_t)bytes + 1);
  if (!text)
    return NULL;
  if (seek(h->journal, (int64_t)offset, SEEK_SET) ||
      (bytes && fread(text, 1, (size_t)bytes, h->journal) != bytes)) {
    free(text);
    return NULL;
  }
  text[bytes] = 0;
  return text;
}
static void inverse(buffer_edit *s) {
  int row = s->old_row, col = s->old_col;
  s->old_row = s->new_row;
  s->old_col = s->new_col;
  s->new_row = row;
  s->new_col = col;
}
static void announce(editor *e, buffer *b, buffer_edit edit) {
  if (b->on_edit)
    b->on_edit(b, &edit, b->edit_context);
  view_rebase(e->view, &edit);
}
static int replay(editor *e, int redo) {
  if (!documents_editable(e))
    return 0;
  document *doc = e->view->doc;
  struct undo_history *h = doc->history;
  undo_group *g = h ? (redo ? h->redo : h->undo) : NULL;
  if (!g) {
    snprintf(e->notice, sizeof e->notice, "Nothing to %s",
             redo ? "redo" : "undo");
    return 0;
  }
  if (g->count > SIZE_MAX / sizeof(buffer_patch *))
    goto error;
  uint64_t hint_count = 0;
  for (undo_edit *r = g->first; r; r = r->next) {
    uint64_t n = r->hints && r->external ? r->hints : 1;
    if (n > SIZE_MAX / sizeof(buffer_edit) - hint_count)
      goto error;
    hint_count += n;
  }
  buffer_edit *events = malloc((size_t)hint_count * sizeof *events);
  buffer_patch **plans = calloc(g->count, sizeof *plans);
  if (!plans || !events) {
    free(plans);
    free(events);
    goto error;
  }
  size_t event_count = 0;
  for (undo_edit *r = redo ? g->first : g->last; r;
       r = redo ? r->next : r->previous) {
    if (r->hints && r->external) {
      for (uint64_t i = 0; i < r->hints; i++) {
        uint64_t n = redo ? i : r->hints - i - 1;
        if (seek(h->journal,
                 (int64_t)(r->hints_offset + n * sizeof(buffer_edit)),
                 SEEK_SET) ||
            fread(&events[event_count], sizeof(buffer_edit), 1, h->journal) !=
                1) {
          free(plans);
          free(events);
          clearerr(h->journal);
          goto error;
        }
        if (!redo)
          inverse(&events[event_count]);
        event_count++;
      }
    } else {
      events[event_count] = r->span;
      if (!redo)
        inverse(&events[event_count]);
      event_count++;
    }
  }
  size_t applied = 0;
  for (undo_edit *r = redo ? g->first : g->last; r;
       r = redo ? r->next : r->previous) {
    uint64_t bytes = redo ? r->new_bytes : r->old_bytes;
    char *text = read_text(h, redo ? r->new_offset : r->old_offset, bytes);
    if (!text)
      goto rollback;
    buffer_edit edit = r->span;
    if (!redo)
      inverse(&edit);
    buffer_patch *p = buffer_patch_prepare(
        &doc->buf, edit.row, edit.col, edit.old_row, edit.old_col, text,
        (size_t)bytes, redo ? r->after_empty : r->before_empty);
    free(text);
    if (!p)
      goto rollback;
    plans[applied++] = p;
    buffer_patch_apply(p);
  }
  // No observer sees speculative edits. Announce only after every splice is
  // ready.
  h->replaying = 1;
  for (size_t i = 0; i < event_count; i++)
    announce(e, &doc->buf, events[i]);
  free(events);
  h->replaying = 0;
  for (size_t i = 0; i < applied; i++)
    buffer_patch_dispose(plans[i], 1);
  free(plans);
  if (redo) {
    h->redo = g->next;
    if (h->redo)
      h->redo->previous = NULL;
    g->previous = h->undo;
    g->next = NULL;
    if (h->undo)
      h->undo->next = g;
    else
      h->oldest = g;
    h->undo = g;
    h->actions++;
    h->state = g->after_state;
  } else {
    h->undo = g->previous;
    if (h->undo)
      h->undo->next = NULL;
    else
      h->oldest = NULL;
    g->previous = NULL;
    g->next = h->redo;
    if (h->redo)
      h->redo->previous = g;
    h->redo = g;
    h->actions--;
    h->state = g->before_state;
  }
  view snapshot = redo ? g->after : g->before;
  rect area = e->view->area;
  unsigned long revision = e->view->revision;
  *e->view = snapshot;
  e->view->doc = doc;
  e->view->used = 1;
  e->view->area = area;
  e->view->revision = revision;
  doc->dirty = h->state != h->saved;
  dispatch_change(e);
  snprintf(e->notice, sizeof e->notice, "%s", redo ? "Redo" : "Undo");
  return 1;
rollback:
  while (applied) {
    buffer_patch_revert(plans[--applied]);
    buffer_patch_dispose(plans[applied], 0);
  }
  free(plans);
  free(events);
  clearerr(h->journal);
error:
  snprintf(e->notice, sizeof e->notice, "Cannot %s; text and history unchanged",
           redo ? "redo" : "undo");
  return 0;
}
static void undo_command(editor *e) { replay(e, 0); }
static void redo_command(editor *e) { replay(e, 1); }
static int can_undo(const editor *e) {
  return documents_editable(e) && e->view->doc->history &&
         e->view->doc->history->undo;
}
static int can_redo(const editor *e) {
  return documents_editable(e) && e->view->doc->history &&
         e->view->doc->history->redo;
}
static void init(editor *e) {
  e->undo = calloc(1, sizeof *e->undo);
  dispatch_bind(CTRL('z'), undo_command);
  dispatch_bind(META('z'), redo_command);
  dispatch_bind_prefix_when('z', undo_command, "z", "Undo", can_undo);
  dispatch_bind_prefix_when('Z', redo_command, "Z", "Redo", can_redo);
  dispatch_pair_prefix('z', 'Z', "Undo / Redo");
}
static void changed(editor *e) {
  if (e->undo && !e->undo->depth)
    commit(e);
}
static void undo_shutdown(editor *e) {
  commit(e);
  free(e->undo);
  e->undo = NULL;
}
static module undo = {
    .name = "undo", .init = init, .on_change = changed, .shutdown = undo_shutdown};
module *undo_module(void) { return &undo; }

void undo_status(const document *doc, undo_stats *stats) {
  *stats = (undo_stats){0};
  if (!doc || !doc->history)
    return;
  const struct undo_history *h = doc->history;
  stats->undo_actions = h->actions;
  for (undo_group *g = h->redo; g; g = g->next)
    stats->redo_actions++;
  for (undo_group *g = h->oldest; g; g = g->next)
    stats->records += g->count;
  stats->journal_bytes = h->offset;
}
#ifdef ADM_TEST_ALLOC
int undo_io_failed;
void undo_test_io_failure(int fail) { undo_io_failed = fail; }
#endif
