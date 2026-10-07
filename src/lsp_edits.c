#include "lsp_edits.h"
#include "dispatch.h"
#include "lsp.h"
#include "path.h"
#include "undo.h"
#include "utf8.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define FILE_LIMIT 128
#define EDIT_LIMIT 8192
#define TEXT_LIMIT (8u * 1024u * 1024u)
#define TOTAL_LIMIT (32u * 1024u * 1024u)
typedef struct edit {
  size_t start, end;
  int sr, sc, er, ec;
  char *text;
} edit;
typedef struct plan {
  char *path, *uri, *before, *after;
  document *doc;
  editor_tab *tab;
  edit *edits;
  size_t count, edit_capacity, line_count;
  size_t *line_offsets;
  buffer_patch *patch;
  buffer_edit *events;
  struct undo_transaction *undo;
  struct stat disk;
  int on_disk, held, position_row, position_unit, position_byte;
} plan;
static char *snapshot(const buffer *b) {
  size_t size = 0;
  for (block *k = b->head; k; k = k->next)
    for (int i = 0; i < k->count; i++) {
      size_t n = strnlen(k->lines[i], TEXT_LIMIT + 1);
      if (n > TEXT_LIMIT || n + 1 > TEXT_LIMIT - size)
        return NULL;
      size += n + 1;
    }
  char *text = malloc(size + 1);
  if (!text)
    return NULL;
  size_t at = 0;
  for (block *k = b->head; k; k = k->next)
    for (int i = 0; i < k->count; i++) {
      size_t n = strlen(k->lines[i]);
      memcpy(text + at, k->lines[i], n);
      at += n;
      text[at++] = '\n';
    }
  text[at] = 0;
  return text;
}
static int parse_position(const json_value *v, size_t token, const plan *p,
                          int *line, int *column) {
  int64_t row, col;
  if (!json_int(v, json_get(v, token, "line"), &row) ||
      !json_int(v, json_get(v, token, "character"), &col) || row < 0 ||
      col < 0 || (uint64_t)row >= p->line_count || col > INT_MAX)
    return 0;
  size_t at = p->line_offsets[row], length = p->line_offsets[row + 1] - at;
  if (length && p->before[at + length - 1] == '\n')
    length--;
  if ((uint64_t)col > length)
    return 0;
  *line = (int)row;
  *column = (int)col;
  return 1;
}
static int position(plan *p, int row, int col, int utf8, size_t *offset) {
  size_t at = p->line_offsets[row];
  const char *line = p->before + at;
  if (utf8) {
    if (((unsigned char)line[col] & 0xc0) == 0x80)
      return 0;
    *offset = at + (size_t)col;
    return 1;
  }
  // Edits are sorted and validated before decoding. Even thousands of edits
  // on one long UTF-16 line therefore scan that line only once.
  if (row != p->position_row || col < p->position_unit) {
    p->position_row = row;
    p->position_unit = p->position_byte = 0;
  }
  while (p->position_unit < col && line[p->position_byte] &&
         line[p->position_byte] != '\n') {
    int cp, n = utf8_decode(line + p->position_byte, &cp);
    int width = cp > 0xffff ? 2 : 1;
    if (width > col - p->position_unit)
      return 0;
    p->position_unit += width;
    p->position_byte += n;
  }
  if (p->position_unit != col)
    return 0;
  *offset = at + (size_t)p->position_byte;
  return 1;
}
static int compare(const void *a, const void *b) {
  const edit *x = a, *y = b;
  if (x->sr != y->sr)
    return x->sr < y->sr ? -1 : 1;
  if (x->sc != y->sc)
    return x->sc < y->sc ? -1 : 1;
  if (x->er != y->er)
    return x->er < y->er ? -1 : 1;
  return x->ec < y->ec ? -1 : x->ec != y->ec;
}
static int same_disk(const struct stat *a, const struct stat *b) {
  if (a->st_size != b->st_size || a->st_mtime != b->st_mtime ||
      a->st_ctime != b->st_ctime || a->st_dev != b->st_dev ||
      a->st_ino != b->st_ino)
    return 0;
#if defined(__APPLE__)
  if (a->st_mtimespec.tv_nsec != b->st_mtimespec.tv_nsec ||
      a->st_ctimespec.tv_nsec != b->st_ctimespec.tv_nsec)
    return 0;
#elif !defined(_WIN32)
  if (a->st_mtim.tv_nsec != b->st_mtim.tv_nsec ||
      a->st_ctim.tv_nsec != b->st_ctim.tv_nsec)
    return 0;
#endif
  return 1;
}
static int add_edits(plan *p, const json_value *v, size_t array,
                     size_t *total) {
  if (!json_is(v, array, JSON_ARRAY))
    return 0;
  for (size_t i = json_first(v, array);
       i != JSON_NONE && i < v->tokens[array].next; i = v->tokens[i].next) {
    if (++*total > EDIT_LIMIT)
      return 0;
    size_t range = json_get(v, i, "range");
    edit a = {0};
    if (!parse_position(v, json_get(v, range, "start"), p, &a.sr, &a.sc) ||
        !parse_position(v, json_get(v, range, "end"), p, &a.er, &a.ec) ||
        a.er < a.sr || (a.er == a.sr && a.ec < a.sc))
      return 0;
    a.text = json_string(v, json_get(v, i, "newText"));
    if (!a.text)
      return 0;
    // Editor buffers consistently use LF line endings.
    size_t n = 0;
    for (size_t j = 0; a.text[j]; j++) {
      if (a.text[j] == '\r' && a.text[j + 1] == '\n')
        continue;
      a.text[n++] = a.text[j];
    }
    a.text[n] = 0;
    if (p->count == p->edit_capacity) {
      size_t capacity = p->edit_capacity ? p->edit_capacity * 2 : 32;
      edit *more = realloc(p->edits, capacity * sizeof *more);
      if (!more) {
        free(a.text);
        return 0;
      }
      p->edits = more;
      p->edit_capacity = capacity;
    }
    p->edits[p->count++] = a;
  }
  return 1;
}
static plan *get_plan(editor *e, plan *plans, size_t *count, const char *uri,
                      const lsp_edit_version *versions, size_t version_count) {
  for (size_t i = 0; i < *count; i++)
    if (!strcmp(plans[i].uri, uri))
      return &plans[i];
  if (*count == FILE_LIMIT)
    return NULL;
  plan *p = &plans[(*count)++];
  p->uri = malloc(strlen(uri) + 1);
  if (!p->uri)
    return NULL;
  strcpy(p->uri, uri);
  p->path = lsp_uri_path(uri);
  if (!p->path)
    return NULL;
  p->doc = documents_find(e, p->path);
  if (p->doc) {
    int known = 0;
    for (size_t i = 0; i < version_count; i++)
      if (!strcmp(versions[i].uri, uri)) {
        known = versions[i].generation == p->doc->change_id;
        break;
      }
    if (!known || p->doc->readonly)
      return NULL;
  } else {
    if (stat(p->path, &p->disk) < 0)
      return NULL;
    if (p->disk.st_size < 0 || (uint64_t)p->disk.st_size > TEXT_LIMIT)
      return NULL;
    p->on_disk = 1;
    p->doc = document_open(p->path);
    if (!p->doc)
      return NULL;
    struct stat after;
    if (stat(p->path, &after) < 0 || !same_disk(&after, &p->disk)) {
      document_retain(p->doc);
      document_release(p->doc);
      p->doc = NULL;
      return NULL;
    }
  }
  document_retain(p->doc);
  p->held = 1;
  // Hard-link/symlink aliases must never apply two incompatible transactions.
  for (size_t i = 0; i + 1 < *count; i++)
    if (plans[i].doc == p->doc || document_matches(plans[i].doc, p->path))
      return NULL;
  p->before = snapshot(&p->doc->buf);
  if (!p->before)
    return NULL;
  p->line_count = 1;
  for (const char *s = p->before; *s;) {
    if ((unsigned char)*s >= 128) {
      int cp, n = utf8_decode(s, &cp);
      if (n == 1)
        return NULL;
      s += n;
    } else {
      if (*s == '\n')
        p->line_count++;
      s++;
    }
  }
  p->line_offsets = malloc((p->line_count + 1) * sizeof *p->line_offsets);
  if (!p->line_offsets)
    return NULL;
  size_t line = 0;
  p->line_offsets[line++] = 0;
  for (size_t i = 0; p->before[i]; i++)
    if (p->before[i] == '\n')
      p->line_offsets[line++] = i + 1;
  p->line_offsets[line] = strlen(p->before);
  size_t aggregate = 0;
  for (size_t i = 0; i < *count; i++)
    if (plans[i].before) {
      size_t n = strlen(plans[i].before);
      if (n > TOTAL_LIMIT - aggregate)
        return NULL;
      aggregate += n;
    }
  return p;
}
static void byte_range(const char *s, size_t at, int *row, int *col) {
  *row = 0;
  *col = 0;
  for (size_t i = 0; i < at; i++) {
    if (s[i] == '\n') {
      (*row)++;
      *col = 0;
    } else
      (*col)++;
  }
}
static int prepare(editor *e, plan *p, int utf8) {
  if (!p->count)
    return 1;
  qsort(p->edits, p->count, sizeof *p->edits, compare);
  size_t old = strlen(p->before), size = old, previous = 0;
  for (size_t i = 0; i < p->count; i++) {
    edit *a = &p->edits[i];
    if (i) {
      edit *last = &p->edits[i - 1];
      if (a->sr < last->er || (a->sr == last->er && a->sc < last->ec) ||
          (a->sr == last->sr && a->sc == last->sc))
        return 0;
    }
    if (!position(p, a->sr, a->sc, utf8, &a->start) ||
        !position(p, a->er, a->ec, utf8, &a->end))
      return 0;
    if (a->start < previous || (i && a->start == p->edits[i - 1].start))
      return 0;
    previous = a->end;
    size_t n = strlen(a->text);
    if (n > TEXT_LIMIT || size - (a->end - a->start) > TEXT_LIMIT - n)
      return 0;
    size = size - (a->end - a->start) + n;
  }
  p->after = malloc(size + 1);
  if (!p->after)
    return 0;
  size_t read = 0, write = 0;
  for (size_t i = 0; i < p->count; i++) {
    edit *a = &p->edits[i];
    size_t n = a->start - read;
    memcpy(p->after + write, p->before + read, n);
    write += n;
    n = strlen(a->text);
    memcpy(p->after + write, a->text, n);
    write += n;
    read = a->end;
  }
  memcpy(p->after + write, p->before + read, old - read + 1);
  size_t protocol_end = old;
  int after_lf = size && p->after[size - 1] == '\n';
  // Protocol snapshots use the same final LF as file_write_buffer. Remove
  // that serialization terminator when mapping back to physical buffer lines;
  // otherwise whole-document formatting would create an extra blank line.
  if (old && p->before[old - 1] == '\n')
    p->before[--old] = 0;
  if (after_lf)
    p->after[--size] = 0;
  if (!strcmp(p->after, p->before))
    return 1;
  p->events = calloc(p->count, sizeof *p->events);
  if (!p->events)
    return 0;
  // Walk the original text once. Announce right-to-left edits so cursors and
  // selections in unchanged text between edits retain their exact positions.
  size_t scan = 0;
  int scan_row = 0, scan_col = 0;
  for (size_t i = 0; i < p->count; i++) {
    edit *a = &p->edits[i];
    size_t start = a->start < old ? a->start : old;
    size_t end = a->end < old ? a->end : old;
    while (scan < start) {
      if (p->before[scan++] == '\n') {
        scan_row++;
        scan_col = 0;
      } else
        scan_col++;
    }
    buffer_edit event = {.row = scan_row, .col = scan_col};
    while (scan < end) {
      if (p->before[scan++] == '\n') {
        scan_row++;
        scan_col = 0;
      } else
        scan_col++;
    }
    event.old_row = scan_row;
    event.old_col = scan_col;
    event.new_row = event.row;
    event.new_col = event.col;
    size_t length = strlen(a->text);
    if (after_lf && i + 1 == p->count && a->end == protocol_end && length &&
        a->text[length - 1] == '\n')
      length--;
    if (a->start > old) {
      event.new_row++;
      event.new_col = 0;
    }
    for (size_t j = 0; j < length; j++) {
      if (a->text[j] == '\n') {
        event.new_row++;
        event.new_col = 0;
      } else
        event.new_col++;
    }
    p->events[p->count - i - 1] = event;
  }
  size_t start = 0, old_end = old, new_end = size;
  while (start < old_end && start < new_end &&
         p->before[start] == p->after[start])
    start++;
  while (start && ((unsigned char)p->after[start] & 0xc0) == 0x80)
    start--;
  while (old_end > start && new_end > start &&
         p->before[old_end - 1] == p->after[new_end - 1]) {
    old_end--;
    new_end--;
  }
  while (old_end < old && ((unsigned char)p->before[old_end] & 0xc0) == 0x80) {
    old_end++;
    new_end++;
  }
  int row, col, er, ec;
  byte_range(p->before, start, &row, &col);
  byte_range(p->before, old_end, &er, &ec);
  p->patch = buffer_patch_prepare(&p->doc->buf, row, col, er, ec,
                                  p->after + start, new_end - start, 0);
  if (!p->patch)
    return 0;
  buffer_edit span = *buffer_patch_edit(p->patch);
  p->undo = undo_prepare(e, p->doc, &span, p->after + start, new_end - start,
                         p->doc->buf.nlines + span.new_row - span.old_row +
                             !p->doc->buf.nlines,
                         p->events, p->count);
  if (!p->undo)
    return 0;
  if (p->on_disk) {
    p->tab = tabs_prepare_document(p->doc);
    if (!p->tab)
      return 0;
  }
  return 1;
}
int lsp_apply_edits(editor *e, const json_value *v, size_t result,
                    const char *format_uri, int utf8,
                    const lsp_edit_version *versions, size_t version_count) {
  plan *plans = calloc(FILE_LIMIT, sizeof *plans);
  if (!plans) {
    snprintf(e->notice, sizeof e->notice,
             "Cannot prepare LSP edits; text unchanged");
    return 0;
  }
  size_t count = 0, total = 0;
  int success = 0;
  if (format_uri) {
    plan *p = get_plan(e, plans, &count, format_uri, versions, version_count);
    if (!p || !add_edits(p, v, result, &total))
      goto done;
  } else {
    size_t docs = json_get(v, result, "documentChanges");
    if (json_is(v, docs, JSON_ARRAY)) {
      for (size_t i = json_first(v, docs);
           i != JSON_NONE && i < v->tokens[docs].next; i = v->tokens[i].next) {
        size_t doc = json_get(v, i, "textDocument");
        char *uri = json_string(v, json_get(v, doc, "uri"));
        if (!uri)
          goto done;
        plan *p = get_plan(e, plans, &count, uri, versions, version_count);
        free(uri);
        if (!p)
          goto done;
        int64_t version;
        size_t token = json_get(v, doc, "version");
        if (json_int(v, token, &version) && (lsp_version(p->doc) != version))
          goto done;
        if (!json_is(v, token, JSON_NULL) && !json_int(v, token, &version))
          goto done;
        // Null versions use the request generation guard; integers must match.
        if (!add_edits(p, v, json_get(v, i, "edits"), &total))
          goto done;
      }
    } else {
      size_t changes = json_get(v, result, "changes");
      if (!json_is(v, changes, JSON_OBJECT))
        goto done;
      for (size_t i = changes + 1; i < v->tokens[changes].next;) {
        char *uri = json_string(v, i);
        if (!uri)
          goto done;
        plan *p = get_plan(e, plans, &count, uri, versions, version_count);
        free(uri);
        if (!p || !add_edits(p, v, i + 1, &total))
          goto done;
        i = v->tokens[i + 1].next;
      }
    }
  }
  for (size_t i = 0; i < count; i++)
    if (!prepare(e, &plans[i], utf8))
      goto done;
  for (size_t i = 0; i < count; i++)
    if (plans[i].on_disk) {
      struct stat current;
      if (stat(plans[i].path, &current) < 0 ||
          !same_disk(&current, &plans[i].disk))
        goto done;
    }
  // No allocation or disk write remains. All files commit together in memory.
  int changed = 0;
  for (size_t i = 0; i < count; i++) {
    plan *p = &plans[i];
    if (!p->patch)
      continue;
    if (p->tab) {
      tabs_commit_document(e, p->tab);
      p->tab = NULL;
    }
    buffer_patch_apply(p->patch);
    buffer *b = &p->doc->buf;
    for (size_t j = 0; j < p->count; j++) {
      if (b->on_edit)
        b->on_edit(b, &p->events[j], b->edit_context);
      if (e->view->doc == p->doc)
        view_rebase(e->view, &p->events[j]);
    }
    undo_commit_prepared(e, p->undo);
    p->undo = NULL;
    buffer_patch_dispose(p->patch, 1);
    p->patch = NULL;
    changed++;
  }
  dispatch_change(e);
  snprintf(e->notice, sizeof e->notice,
           "LSP: %d file%s changed; save buffers to write to disk", changed,
           changed == 1 ? "" : "s");
  success = 1;
done:
  for (size_t i = 0; i < count; i++) {
    plan *p = &plans[i];
    buffer_patch_dispose(p->patch, 0);
    undo_discard_prepared(p->undo);
    tabs_discard_document(p->tab);
    if (p->held)
      document_release(p->doc);
    for (size_t j = 0; j < p->count; j++)
      free(p->edits[j].text);
    free(p->edits);
    free(p->events);
    free(p->path);
    free(p->uri);
    free(p->before);
    free(p->line_offsets);
    free(p->after);
  }
  free(plans);
  if (!success)
    snprintf(e->notice, sizeof e->notice,
             "LSP edits rejected: stale, overlapping, unsupported or "
             "unavailable files; text unchanged");
  return success;
}
