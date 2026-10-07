#include "theme.h"
#include "lsp.h"
#include "cursor.h"
#include "dispatch.h"
#include "json.h"
#include "lsp_edits.h"
#include "path.h"
#include "process.h"
#include "runtime.h"
#include "screen.h"
#include "utf8.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SESSION_LIMIT 8
#define DOCUMENT_LIMIT (1024u * 1024u)
#define MESSAGE_LIMIT (8u * 1024u * 1024u - 4096)
#define RESULT_LIMIT 128
#define DIAGNOSTIC_LIMIT 256
#define REQUEST_TIMEOUT 15000

typedef struct diagnostic {
  int row, col, severity;
  char *message;
} diagnostic;
typedef struct lsp_session lsp_session;
struct lsp_document {
  struct lsp_document *next;
  document *doc;
  lsp_session *server;
  char *uri, *snapshot;
  size_t bytes;
  unsigned long long generation;
  int version, opened, paused, save_pending;
  uint64_t changed_at;
  diagnostic diagnostics[DIAGNOSTIC_LIMIT];
  size_t diagnostic_count;
};
struct lsp_session {
  const language *language;
  child_process process;
  struct lsp_document *documents;
  int state, sync, utf8, hover, definition, completion, rename, formatting,
      save, save_text;
  int64_t initialize_id, shutdown_id;
  int suppressed, restart;
  uint64_t deadline;
  char error[160];
};
typedef struct lsp_item {
  char *label, *text, *uri;
  int sr, sc, er, ec, range, unsupported;
} lsp_item;
struct lsp_state {
  lsp_session sessions[SESSION_LIMIT];
  int menu, scroll, popup, selected, automatic;
  lsp_item items[RESULT_LIMIT];
  size_t count;
  int64_t next_id, request_id;
  int request_kind;
  lsp_session *request_server;
  char *request_uri;
  unsigned long tab, revision;
  unsigned long long generation;
  int row, col;
  uint64_t request_deadline;
  char *hover;
  int name_prompt, name_length, name_cursor;
  char new_name[256];
  lsp_edit_version versions[128];
  size_t version_count;
};
static char *copy(const char *s) {
  char *p = malloc(strlen(s) + 1);
  if (p)
    strcpy(p, s);
  return p;
}
int lsp_position_units(const char *s, int bytes, int utf8) {
  int at = 0, units = 0;
  while (s && s[at] && at < bytes) {
    int cp, n = utf8_decode(s + at, &cp);
    if (at + n > bytes)
      break;
    units += utf8 ? n : cp > 0xffff ? 2 : 1;
    at += n;
  }
  return units;
}
int lsp_position_bytes(const char *s, int units, int utf8) {
  int at = 0, used = 0;
  while (s && s[at]) {
    int cp, n = utf8_decode(s + at, &cp),
            width = utf8          ? n
                    : cp > 0xffff ? 2
                                  : 1;
    if (width > units - used)
      break;
    used += width;
    at += n;
  }
  return at;
}
#ifdef _WIN32
// The existing filesystem layer uses native narrow paths. Protocol URIs and
// JSON always use UTF-8; convert explicitly instead of assuming an ANSI locale.
static char *convert_path(const char *text, UINT from, UINT to) {
  int n = MultiByteToWideChar(from, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
  if (!n)
    return NULL;
  wchar_t *wide = malloc((size_t)n * sizeof *wide);
  if (!wide)
    return NULL;
  if (!MultiByteToWideChar(from, MB_ERR_INVALID_CHARS, text, -1, wide, n)) {
    free(wide);
    return NULL;
  }
  BOOL used_default = FALSE;
  int utf8 = to == CP_UTF8 || (to == CP_ACP && GetACP() == CP_UTF8);
  DWORD flags = utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
  BOOL *used = utf8 ? NULL : &used_default;
  int bytes = WideCharToMultiByte(to, flags, wide, -1, NULL, 0, NULL, used);
  char *result = bytes ? malloc((size_t)bytes) : NULL;
  if (result &&
      (!WideCharToMultiByte(to, flags, wide, -1, result, bytes, NULL, used) ||
       used_default)) {
    free(result);
    result = NULL;
  }
  free(wide);
  return result;
}
#endif
char *lsp_uri(const char *path) {
  char *absolute = path_absolute(path);
  if (!absolute)
    return NULL;
#ifdef _WIN32
  char *encoded = convert_path(absolute, CP_ACP, CP_UTF8);
  free(absolute);
  absolute = encoded;
  if (!absolute)
    return NULL;
#endif
  json_buf b = {0};
  json_add(&b, "file://");
#ifdef _WIN32
  if (absolute[0] != '/')
    json_add(&b, "/");
#endif
  const char *hex = "0123456789ABCDEF";
  for (char *p = absolute; *p; p++) {
    unsigned char c = (unsigned char)*p;
#ifdef _WIN32
    if (c == '\\')
      c = '/';
#endif
    if ((isalnum(c) && c < 128) || c == '/' || c == ':' || c == '-' ||
        c == '_' || c == '.' || c == '~')
      json_bytes(&b, (char *)&c, 1);
    else {
      char code[3] = {'%', hex[c >> 4], hex[c & 15]};
      json_bytes(&b, code, 3);
    }
  }
  free(absolute);
  if (b.failed) {
    json_buf_free(&b);
    return NULL;
  }
  return b.data;
}
static int hex(char c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                : -1;
}
char *lsp_uri_path(const char *uri) {
  if (strncmp(uri, "file://", 7))
    return NULL;
  const char *p = uri + 7;
  if (!strncmp(p, "localhost/", 10))
    p += 9;
  if (*p != '/')
    return NULL; // Refuse remote authorities and non-file schemes.
#ifdef _WIN32
  if (isalpha((unsigned char)p[1]) && p[2] == ':')
    p++;
#endif
  char *path = malloc(strlen(p) + 1);
  if (!path)
    return NULL;
  size_t n = 0;
  while (*p) {
    if (*p == '%') {
      if (!p[1] || !p[2] || hex(p[1]) < 0 || hex(p[2]) < 0) {
        free(path);
        return NULL;
      }
      char c = (char)((hex(p[1]) << 4) | hex(p[2]));
      if (!c) {
        free(path);
        return NULL;
      }
      path[n++] = c;
      p += 3;
    } else if (*p == '?' || *p == '#') {
      free(path);
      return NULL;
    } else
      path[n++] = *p++;
  }
  path[n] = 0;
#ifdef _WIN32
  char *native = convert_path(path, CP_UTF8, CP_ACP);
  free(path);
  path = native;
#endif
  return path;
}
static void clear_diagnostics(struct lsp_document *d) {
  for (size_t i = 0; i < d->diagnostic_count; i++)
    free(d->diagnostics[i].message);
  d->diagnostic_count = 0;
}
size_t lsp_diagnostic_count(const document *doc) {
  return doc && doc->lsp && doc->lsp->generation == doc->change_id
             ? doc->lsp->diagnostic_count
             : 0;
}
int lsp_version(const document *doc) {
  return doc && doc->lsp && doc->lsp->opened ? doc->lsp->version : -1;
}
static void clear_versions(struct lsp_state *l) {
  for (size_t i = 0; i < l->version_count; i++)
    free((char *)l->versions[i].uri);
  l->version_count = 0;
}
static int send_message(lsp_session *s, const char *method, int64_t id,
                        json_buf *params) {
  json_buf b = {0};
  json_add(&b, "{\"jsonrpc\":\"2.0\",");
  if (id) {
    json_add(&b, "\"id\":");
    json_number(&b, id);
    json_add(&b, ",");
  }
  json_add(&b, "\"method\":");
  json_quote(&b, method);
  if (strcmp(method, "shutdown") && strcmp(method, "exit")) {
    json_add(&b, ",\"params\":");
    json_add(&b, params && params->data ? params->data : "{}");
  }
  json_add(&b, "}");
  int result = 0;
  if (b.failed || (params && params->failed))
    result = -1;
  else {
    char header[80];
    int n = snprintf(header, sizeof header, "Content-Length: %zu\r\n\r\n",
                     b.length);
    if (b.length + (size_t)n > 8u * 1024u * 1024u - s->process.in.length)
      result = -1;
    else if (process_send(&s->process, header, (size_t)n) < 0 ||
             process_send(&s->process, b.data, b.length) < 0)
      result = -1;
  }
  if (result < 0) {
    s->state = 3;
    snprintf(s->error, sizeof s->error,
             "LSP send failed (server stopped or queue full)");
  }
  json_buf_free(&b);
  return result;
}
static void text_document(json_buf *b, const char *uri) {
  json_add(b, "{\"uri\":");
  json_quote(b, uri);
  json_add(b, "}");
}
static void close_document(struct lsp_document *d) {
  if (d->opened && d->server->state == 2) {
    json_buf b = {0};
    json_add(&b, "{\"textDocument\":");
    text_document(&b, d->uri);
    json_add(&b, "}");
    send_message(d->server, "textDocument/didClose", 0, &b);
    json_buf_free(&b);
  }
  d->opened = 0;
  free(d->snapshot);
  d->snapshot = NULL;
  d->bytes = 0;
  clear_diagnostics(d);
}
void lsp_detach(document *doc) {
  if (!doc || !doc->lsp)
    return;
  struct lsp_document *d = doc->lsp;
  close_document(d);
  struct lsp_document **link = &d->server->documents;
  while (*link && *link != d)
    link = &(*link)->next;
  if (*link)
    *link = d->next;
  free(d->uri);
  free(d);
  doc->lsp = NULL;
}
void lsp_saved(document *doc) {
  if (doc && doc->lsp)
    doc->lsp->save_pending = 1;
}
static char *snapshot(document *doc, size_t *bytes) {
  size_t n = 0;
  for (block *k = doc->buf.head; k; k = k->next)
    for (int i = 0; i < k->count; i++) {
      size_t length = strnlen(k->lines[i], DOCUMENT_LIMIT + 1);
      if (length > DOCUMENT_LIMIT || length + 1 > DOCUMENT_LIMIT - n)
        return NULL;
      n += length + 1;
    }
  char *text = malloc(n + 1);
  if (!text)
    return NULL;
  size_t at = 0;
  for (block *k = doc->buf.head; k; k = k->next)
    for (int i = 0; i < k->count; i++) {
      const char *line = k->lines[i];
      for (int j = 0; line[j];) {
        int cp, len = utf8_decode(line + j, &cp);
        if (len == 1 && (unsigned char)line[j] >= 128) {
          free(text);
          return NULL;
        }
        j += len;
      }
      size_t length = strlen(line);
      memcpy(text + at, line, length);
      at += length;
      // Match file_write_buffer: one LF follows each physical buffer line.
      text[at++] = '\n';
    }
  text[at] = 0;
  *bytes = at;
  return text;
}
static void position(json_buf *b, const char *text, size_t offset, int utf8) {
  int row = 0;
  size_t start = 0;
  for (size_t i = 0; i < offset; i++)
    if (text[i] == '\n') {
      row++;
      start = i + 1;
    }
  json_add(b, "{\"line\":");
  json_number(b, row);
  json_add(b, ",\"character\":");
  json_number(b, lsp_position_units(text + start, (int)(offset - start), utf8));
  json_add(b, "}");
}
static int sync_document(struct lsp_document *d) {
  document *doc = d->doc;
  lsp_session *s = d->server;
  size_t bytes = 0;
  char *text = snapshot(doc, &bytes);
  if (!text) {
    close_document(d);
    d->paused = 1;
    d->generation = doc->change_id;
    return 0;
  }
  json_buf b = {0};
  json_add(&b, "{\"textDocument\":{\"uri\":");
  json_quote(&b, d->uri);
  json_add(&b, ",\"version\":");
  json_number(&b, ++d->version);
  if (!d->opened) {
    const language *lang = language_for_filename(doc->filename);
    json_add(&b, ",\"languageId\":");
    json_quote(&b, lang->id);
    json_add(&b, ",\"text\":");
    json_quote_bytes(&b, text, bytes);
    json_add(&b, "}}");
    if (send_message(s, "textDocument/didOpen", 0, &b) < 0)
      goto fail;
    d->opened = 1;
  } else {
    json_add(&b, "},\"contentChanges\":[{");
    size_t start = 0, old_end = d->bytes, new_end = bytes;
    if (s->sync == 2) {
      while (start < old_end && start < new_end &&
             d->snapshot[start] == text[start])
        start++;
      while (start && ((unsigned char)text[start] & 0xc0) == 0x80)
        start--;
      while (old_end > start && new_end > start &&
             d->snapshot[old_end - 1] == text[new_end - 1]) {
        old_end--;
        new_end--;
      }
      while (old_end < d->bytes &&
             ((unsigned char)d->snapshot[old_end] & 0xc0) == 0x80) {
        old_end++;
        new_end++;
      }
      json_add(&b, "\"range\":{\"start\":");
      position(&b, d->snapshot, start, s->utf8);
      json_add(&b, ",\"end\":");
      position(&b, d->snapshot, old_end, s->utf8);
      json_add(&b, "},");
    } else {
      start = 0;
      new_end = bytes;
    }
    json_add(&b, "\"text\":");
    json_quote_bytes(&b, text + start, new_end - start);
    json_add(&b, "}]}");
    if (s->sync && send_message(s, "textDocument/didChange", 0, &b) < 0)
      goto fail;
    if (!s->sync) {
      free(text);
      json_buf_free(&b);
      return 0;
    }
  }
  free(d->snapshot);
  d->snapshot = text;
  d->bytes = bytes;
  d->generation = doc->change_id;
  d->paused = 0;
  clear_diagnostics(d);
  json_buf_free(&b);
  return 1;
fail:
  free(text);
  json_buf_free(&b);
  return 0;
}
static lsp_session *session(editor *e, const language *lang, int create) {
  if (!e->lsp || !lang)
    return NULL;
  lsp_session *empty = NULL;
  for (int i = 0; i < SESSION_LIMIT; i++) {
    lsp_session *s = &e->lsp->sessions[i];
    if (s->language && !strcmp(s->language->server_key, lang->server_key))
      return s;
    if (!s->language && !empty)
      empty = s;
  }
  if (create && empty) {
    empty->language = lang;
    return empty;
  }
  return NULL;
}
static struct lsp_document *attach(lsp_session *s, document *doc) {
  if (doc->lsp)
    return doc->lsp;
  struct lsp_document *d = calloc(1, sizeof *d);
  if (!d)
    return NULL;
  d->uri = lsp_uri(doc->filename);
  if (!d->uri) {
    free(d);
    return NULL;
  }
  d->server = s;
  d->doc = doc;
  d->generation = doc->change_id;
  d->next = s->documents;
  s->documents = d;
  doc->lsp = d;
  return d;
}
static void start_server(editor *e, lsp_session *s) {
  if (!s || s->state == 1 || s->state == 2)
    return;
  if (s->state == 4 || s->state == 5) {
    s->restart = 1;
    return;
  }
  s->suppressed = 0;
  s->restart = 0;
  const char *argv[16];
  int count = 0;
  for (; s->language->server_argv[count] && count < 15; count++)
    argv[count] = s->language->server_argv[count];
  argv[count] = NULL;
  const char *override = getenv(s->language->server_env);
  if (override && *override)
    argv[0] = override;
  char *script =
      s->language->server_script ? s->language->server_script(override) : NULL;
  if (script) {
    // Reserve one extra argument for the script before the fixed flags.
    if (count == 15) {
      free(script);
      s->state = 3;
      return;
    }
    for (int i = count; i >= 1; i--)
      argv[i + 1] = argv[i];
    argv[0] = "node";
    argv[1] = script;
  }
  if (process_start_duplex(&s->process, argv) < 0) {
    s->state = 3;
    snprintf(s->error, sizeof s->error, "Cannot start %s", argv[0]);
    free(script);
    return;
  }
  free(script);
  s->state = 1;
  s->error[0] = 0;
  s->initialize_id = ++e->lsp->next_id;
  s->deadline = adm_milliseconds() + REQUEST_TIMEOUT;
  char *root = lsp_uri(e->files.workspace_root);
  json_buf b = {0};
  json_add(&b, "{\"processId\":null,\"rootUri\":");
  if (root)
    json_quote(&b, root);
  else
    json_add(&b, "null");
  json_add(&b, ",\"workspaceFolders\":[{\"uri\":");
  json_quote(&b, root ? root : "");
  json_add(&b, ",\"name\":");
#ifdef _WIN32
  char *root_name =
      convert_path(path_name(e->files.workspace_root), CP_ACP, CP_UTF8);
  json_quote(&b, root_name ? root_name : "workspace");
  free(root_name);
#else
  json_quote(&b, path_name(e->files.workspace_root));
#endif
  json_add(&b,
           "}],\"clientInfo\":{\"name\":\"adm\"},\"capabilities\":{\"general\":"
           "{\"positionEncodings\":[\"utf-8\",\"utf-16\"]},\"textDocument\":{"
           "\"synchronization\":{\"dynamicRegistration\":false,\"didSave\":"
           "true},\"hover\":{\"contentFormat\":[\"plaintext\",\"markdown\"]},"
           "\"completion\":{\"completionItem\":{\"snippetSupport\":false,"
           "\"insertReplaceSupport\":true}},\"publishDiagnostics\":{"
           "\"versionSupport\":true}}}}");
  send_message(s, "initialize", s->initialize_id, &b);
  json_buf_free(&b);
  free(root);
}
static void clear_popup(struct lsp_state *l) {
  for (size_t i = 0; i < l->count; i++) {
    free(l->items[i].label);
    free(l->items[i].text);
    free(l->items[i].uri);
  }
  l->count = 0;
  free(l->hover);
  l->hover = NULL;
  l->popup = l->selected = l->scroll = 0;
}
static void cancel_request(struct lsp_state *l) {
  if (l->request_id && l->request_server && l->request_server->state == 2) {
    json_buf b = {0};
    json_add(&b, "{\"id\":");
    json_number(&b, l->request_id);
    json_add(&b, "}");
    send_message(l->request_server, "$/cancelRequest", 0, &b);
    json_buf_free(&b);
  }
  clear_versions(l);
  free(l->request_uri);
  l->request_uri = NULL;
  l->request_id = 0;
  l->request_server = NULL;
}
static void stop_server(editor *e, lsp_session *s) {
  if (!s)
    return;
  if (e->lsp->request_server == s)
    cancel_request(e->lsp);
  while (s->documents)
    lsp_detach(s->documents->doc);
  s->suppressed = 1;
  if (s->state == 4 || s->state == 5)
    return;
  if (s->state == 2) {
    s->shutdown_id = ++e->lsp->next_id;
    if (send_message(s, "shutdown", s->shutdown_id, NULL) == 0) {
      s->state = 4;
      s->deadline = adm_milliseconds() + 1000;
      return;
    }
  }
  process_cancel(&s->process);
  s->state = 0;
}
static struct lsp_document *find_uri(lsp_session *s, const char *uri) {
  for (struct lsp_document *d = s->documents; d; d = d->next)
    if (!strcmp(d->uri, uri))
      return d;
  return NULL;
}
static int number(const json_value *v, size_t parent, const char *key,
                  int fallback) {
  int64_t n;
  return json_int(v, json_get(v, parent, key), &n) && n >= 0 && n <= INT_MAX
             ? (int)n
             : fallback;
}
static int range(const json_value *v, size_t token, lsp_item *item) {
  size_t a = json_get(v, token, "start"), b = json_get(v, token, "end");
  int sr = number(v, a, "line", -1), sc = number(v, a, "character", -1),
      er = number(v, b, "line", -1), ec = number(v, b, "character", -1);
  if (sr < 0 || sc < 0 || er < sr || ec < 0 || (er == sr && ec < sc))
    return 0;
  item->sr = sr;
  item->sc = sc;
  item->er = er;
  item->ec = ec;
  item->range = 1;
  return 1;
}
static int request_current(editor *e) {
  struct lsp_state *l = e->lsp;
  return e->active_tab->id == l->tab && e->view->revision == l->revision &&
         e->view->doc && e->view->doc->lsp && l->request_uri &&
         !strcmp(e->view->doc->lsp->uri, l->request_uri) &&
         e->view->doc->change_id == l->generation && e->view->cy == l->row &&
         e->view->cx == l->col;
}
static void request(editor *e, int kind) {
  document *doc = e->view->doc;
  const language *lang = doc ? language_for_filename(doc->filename) : NULL;
  lsp_session *s = session(e, lang, 0);
  if (!s || s->state != 2) {
    snprintf(e->notice, sizeof e->notice,
             "Start the language server with M-x s first");
    return;
  }
  if ((kind == 1 && !s->hover) || (kind == 2 && !s->definition) ||
      (kind == 3 && !s->completion) || (kind == 4 && !s->rename) ||
      (kind == 5 && !s->formatting)) {
    snprintf(e->notice, sizeof e->notice,
             "Language server does not support this command");
    return;
  }
  struct lsp_document *d = attach(s, doc);
  if (!d ||
      ((!d->opened || d->generation != doc->change_id) && !sync_document(d))) {
    snprintf(e->notice, sizeof e->notice,
             "LSP unavailable: document exceeds 1 MiB, has invalid UTF-8, or "
             "cannot sync");
    return;
  }
  cancel_request(e->lsp);
  clear_popup(e->lsp);
  struct lsp_state *l = e->lsp;
  if (kind == 4) {
    // Include every visible document in the family, even if a command was
    // entered before the next idle tick attached a newly opened split.
    for (editor_tab *tab = e->tabs; tab; tab = tab->next) {
      workspace *w = tabs_workspace(e, tab);
      for (int i = 0; i < MAX_PANES; i++) {
        document *visible = w->panes[i].doc;
        const language *other = visible && !visible->readonly
                                    ? language_for_filename(visible->filename)
                                    : NULL;
        if (other && !strcmp(other->server_key, s->language->server_key) &&
            !attach(s, visible)) {
          snprintf(e->notice, sizeof e->notice, "Cannot prepare rename files");
          return;
        }
      }
    }
  }
  for (struct lsp_document *other = s->documents; other; other = other->next) {
    if (kind != 4 && other != d)
      continue;
    if ((!other->opened || other->generation != other->doc->change_id) &&
        !sync_document(other)) {
      // A paused, unrelated large document must not disable rename elsewhere.
      // If a result later targets it, the missing version guard rejects all
      // edits.
      if (other != d && other->paused && s->state == 2)
        continue;
      clear_versions(l);
      snprintf(e->notice, sizeof e->notice,
               "Cannot synchronize all rename files");
      return;
    }
    if (l->version_count == 128) {
      clear_versions(l);
      snprintf(e->notice, sizeof e->notice, "Too many files for one LSP edit");
      return;
    }
    char *uri = copy(other->uri);
    if (!uri) {
      clear_versions(l);
      return;
    }
    l->versions[l->version_count++] =
        (lsp_edit_version){uri, other->doc->change_id};
  }
  l->request_uri = copy(d->uri);
  if (!l->request_uri)
    return;
  l->request_id = ++l->next_id;
  l->request_kind = kind;
  l->request_server = s;
  l->tab = e->active_tab->id;
  l->revision = e->view->revision;
  l->generation = doc->change_id;
  l->row = e->view->cy;
  l->col = e->view->cx;
  l->request_deadline = adm_milliseconds() + REQUEST_TIMEOUT;
  json_buf b = {0};
  json_add(&b, "{\"textDocument\":");
  text_document(&b, d->uri);
  if (kind != 5) {
    json_add(&b, ",\"position\":{\"line\":");
    json_number(&b, l->row);
    json_add(&b, ",\"character\":");
    json_number(&b, lsp_position_units(buffer_line(&doc->buf, l->row), l->col,
                                       s->utf8));
    json_add(&b, "}");
  }
  if (kind == 4) {
    json_add(&b, ",\"newName\":");
    json_quote(&b, l->new_name);
  }
  if (kind == 5)
    json_add(&b, ",\"options\":{\"tabSize\":4,\"insertSpaces\":true}");
  json_add(&b, "}");
  const char *method = kind == 1   ? "textDocument/hover"
                       : kind == 2 ? "textDocument/definition"
                       : kind == 3 ? "textDocument/completion"
                       : kind == 4 ? "textDocument/rename"
                                   : "textDocument/formatting";
  send_message(s, method, l->request_id, &b);
  json_buf_free(&b);
  snprintf(e->notice, sizeof e->notice, "LSP request pending; Esc cancels");
}
static void hover_text(const json_value *v, size_t i, json_buf *b) {
  if (json_is(v, i, JSON_STRING)) {
    char *s = json_string(v, i);
    if (s) {
      json_add(b, s);
      free(s);
    }
  } else if (json_is(v, i, JSON_OBJECT))
    hover_text(v, json_get(v, i, "value"), b);
  else if (json_is(v, i, JSON_ARRAY))
    for (size_t n = json_first(v, i); n != JSON_NONE && n < v->tokens[i].next;
         n = v->tokens[n].next) {
      if (b->length)
        json_add(b, "\n");
      hover_text(v, n, b);
    }
}
static void completion_result(editor *e, const json_value *v, size_t result) {
  struct lsp_state *l = e->lsp;
  size_t items =
      json_is(v, result, JSON_ARRAY) ? result : json_get(v, result, "items");
  for (size_t i = json_first(v, items);
       i != JSON_NONE && i < v->tokens[items].next && l->count < RESULT_LIMIT;
       i = v->tokens[i].next) {
    lsp_item item = {0};
    item.label = json_string(v, json_get(v, i, "label"));
    if (!item.label)
      continue;
    size_t edit = json_get(v, i, "textEdit");
    item.text = json_string(v, json_get(v, edit, "newText"));
    if (json_is(v, edit, JSON_OBJECT)) {
      if (!range(v, json_get(v, edit, "range"), &item) &&
          !range(v, json_get(v, edit, "replace"), &item))
        item.unsupported = 1;
    }
    if (!item.text)
      item.text = json_string(v, json_get(v, i, "insertText"));
    if (!item.text)
      item.text = copy(item.label);
    if (!item.text || number(v, i, "insertTextFormat", 1) == 2 ||
        (json_first(v, json_get(v, i, "additionalTextEdits")) != JSON_NONE))
      item.unsupported = 1;
    l->items[l->count++] = item;
  }
  l->popup = 3;
}
static void definition_item(struct lsp_state *l, const json_value *v,
                            size_t i) {
  if (l->count == RESULT_LIMIT)
    return;
  lsp_item item = {0};
  item.uri = json_string(v, json_get(v, i, "uri"));
  size_t r = json_get(v, i, "range");
  if (!item.uri) {
    item.uri = json_string(v, json_get(v, i, "targetUri"));
    r = json_get(v, i, "targetSelectionRange");
  }
  if (!item.uri || !range(v, r, &item)) {
    free(item.uri);
    return;
  }
  char *path = lsp_uri_path(item.uri);
  if (!path) {
    free(item.uri);
    return;
  }
  json_buf b = {0};
  json_add(&b, path);
  json_add(&b, ":");
  json_number(&b, item.sr + 1);
  item.label = b.data;
  free(path);
  if (b.failed) {
    json_buf_free(&b);
    free(item.uri);
    return;
  }
  l->items[l->count++] = item;
}
static void diagnostics(lsp_session *s, const json_value *v, size_t params) {
  char *uri = json_string(v, json_get(v, params, "uri"));
  if (!uri)
    return;
  struct lsp_document *d = find_uri(s, uri);
  free(uri);
  int64_t version;
  if (!d || !d->opened || d->generation != d->doc->change_id ||
      (json_int(v, json_get(v, params, "version"), &version) &&
       version != d->version))
    return;
  clear_diagnostics(d);
  size_t array = json_get(v, params, "diagnostics");
  for (size_t i = json_first(v, array);
       i != JSON_NONE && i < v->tokens[array].next &&
       d->diagnostic_count < DIAGNOSTIC_LIMIT;
       i = v->tokens[i].next) {
    lsp_item item = {0};
    if (!range(v, json_get(v, i, "range"), &item) ||
        item.sr >= d->doc->buf.nlines)
      continue;
    char *message = json_string(v, json_get(v, i, "message"));
    if (!message)
      continue;
    if (strlen(message) > 2048)
      message[2048] = 0;
    const char *line = buffer_line(&d->doc->buf, item.sr);
    diagnostic a = {item.sr, lsp_position_bytes(line, item.sc, s->utf8),
                    number(v, i, "severity", 1), message};
    d->diagnostics[d->diagnostic_count++] = a;
  }
}
static int capability(const json_value *v, size_t caps, const char *key) {
  size_t i = json_get(v, caps, key);
  return json_is(v, i, JSON_OBJECT) || json_true(v, i);
}
static void server_request(lsp_session *s, const json_value *v, size_t id,
                           const char *method) {
  json_buf b = {0};
  json_add(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
  const json_token *token = &v->tokens[id];
  json_bytes(&b, v->text + token->start, token->end - token->start);
  if (!strcmp(method, "workspace/configuration")) {
    size_t params = json_get(v, 0, "params"),
           items = json_get(v, params, "items");
    json_add(&b, ",\"result\":[");
    int n = 0;
    for (size_t i = json_first(v, items);
         i != JSON_NONE && i < v->tokens[items].next; i = v->tokens[i].next) {
      json_add(&b, n++ ? ",null" : "null");
    }
    json_add(&b, "]}");
  } else if (!strcmp(method, "workspace/applyEdit"))
    json_add(&b, ",\"result\":{\"applied\":false,\"failureReason\":\"Workspace "
                 "edits are not supported\"}}");
  else if (!strcmp(method, "window/workDoneProgress/create") ||
           !strcmp(method, "client/registerCapability") ||
           !strcmp(method, "client/unregisterCapability"))
    json_add(&b, ",\"result\":null}");
  else
    json_add(
        &b,
        ",\"error\":{\"code\":-32601,\"message\":\"Method not supported\"}}");
  if (!b.failed) {
    char header[80];
    int n = snprintf(header, sizeof header, "Content-Length: %zu\r\n\r\n",
                     b.length);
    if (process_send(&s->process, header, (size_t)n) < 0 ||
        process_send(&s->process, b.data, b.length) < 0)
      s->state = 3;
  } else
    s->state = 3;
  json_buf_free(&b);
}
static void message(editor *e, lsp_session *s, const char *body, size_t bytes) {
  json_value v = {0};
  if (!json_parse(&v, body, bytes) || !json_is(&v, 0, JSON_OBJECT)) {
    json_free(&v);
    s->state = 3;
    snprintf(s->error, sizeof s->error, "Invalid JSON from language server");
    return;
  }
  size_t id = json_get(&v, 0, "id"), result = json_get(&v, 0, "result"),
         error = json_get(&v, 0, "error");
  char *method = json_string(&v, json_get(&v, 0, "method"));
  int64_t request_id = 0;
  json_int(&v, id, &request_id);
  if (method) {
    if (id != JSON_NONE)
      server_request(s, &v, id, method);
    else if (!strcmp(method, "textDocument/publishDiagnostics"))
      diagnostics(s, &v, json_get(&v, 0, "params"));
    free(method);
  } else if (request_id && request_id == s->shutdown_id && s->state == 4) {
    send_message(s, "exit", 0, NULL);
    s->state = 5;
    s->deadline = adm_milliseconds() + 1000;
  } else if (request_id && request_id == s->initialize_id && s->state == 1) {
    size_t caps = json_get(&v, result, "capabilities");
    if (error != JSON_NONE || !json_is(&v, caps, JSON_OBJECT)) {
      s->state = 3;
      snprintf(s->error, sizeof s->error,
               "Language server initialization failed");
    } else {
      char *encoding = json_string(&v, json_get(&v, caps, "positionEncoding"));
      if (encoding && strcmp(encoding, "utf-8") && strcmp(encoding, "utf-16")) {
        s->state = 3;
        snprintf(s->error, sizeof s->error,
                 "Unsupported LSP position encoding");
      } else {
        s->utf8 = encoding && !strcmp(encoding, "utf-8");
        size_t sync = json_get(&v, caps, "textDocumentSync");
        int64_t n = 0;
        s->sync =
            json_int(&v, sync, &n) ? (int)n : number(&v, sync, "change", 0);
        if (s->sync < 0 || s->sync > 2)
          s->sync = 0;
        size_t save = json_get(&v, sync, "save");
        s->save = json_true(&v, save) || json_is(&v, save, JSON_OBJECT);
        s->save_text = json_true(&v, json_get(&v, save, "includeText"));
        s->hover = capability(&v, caps, "hoverProvider");
        s->definition = capability(&v, caps, "definitionProvider");
        s->completion = capability(&v, caps, "completionProvider");
        s->rename = capability(&v, caps, "renameProvider");
        s->formatting = capability(&v, caps, "documentFormattingProvider");
        s->state = 2;
        send_message(s, "initialized", 0, NULL);
      }
      free(encoding);
    }
  } else if (request_id && request_id == e->lsp->request_id &&
             s == e->lsp->request_server) {
    int current = request_current(e);
    if (current) {
      clear_popup(e->lsp);
      if (error != JSON_NONE) {
        char *text = json_string(&v, json_get(&v, error, "message"));
        snprintf(e->notice, sizeof e->notice, "LSP: %s",
                 text ? text : "Request failed");
        free(text);
      } else if (e->lsp->request_kind == 1) {
        json_buf text = {0};
        hover_text(&v, json_get(&v, result, "contents"), &text);
        e->lsp->hover = text.data;
        e->lsp->popup = text.length && !text.failed ? 1 : 0;
        snprintf(e->notice, sizeof e->notice, "%s",
                 e->lsp->popup ? "" : "No symbol information");
      } else if (e->lsp->request_kind == 2) {
        if (json_is(&v, result, JSON_ARRAY))
          for (size_t i = json_first(&v, result);
               i != JSON_NONE && i < v.tokens[result].next;
               i = v.tokens[i].next)
            definition_item(e->lsp, &v, i);
        else
          definition_item(e->lsp, &v, result);
        e->lsp->popup = e->lsp->count ? 2 : 0;
        snprintf(e->notice, sizeof e->notice, "%s",
                 e->lsp->count ? "" : "No definition found");
      } else if (e->lsp->request_kind == 4 || e->lsp->request_kind == 5) {
        if (json_is(&v, result, JSON_NULL))
          snprintf(e->notice, sizeof e->notice, "No LSP edits");
        else
          lsp_apply_edits(e, &v, result,
                          e->lsp->request_kind == 5 ? e->lsp->request_uri
                                                    : NULL,
                          s->utf8, e->lsp->versions, e->lsp->version_count);
      } else {
        completion_result(e, &v, result);
        if (!e->lsp->count) {
          e->lsp->popup = 0;
          snprintf(e->notice, sizeof e->notice, "No completions");
        } else
          e->notice[0] = 0;
      }
    }
    if (!current || !e->lsp->popup) {
      free(e->lsp->request_uri);
      e->lsp->request_uri = NULL;
    }
    clear_versions(e->lsp);
    e->lsp->request_id = 0;
  }
  json_free(&v);
}
// Handles partial headers/payloads, multiple frames and bounded server output.
static void receive(editor *e, lsp_session *s) {
  process_stream *out = &s->process.out;
  int handled = 0;
  while (out->length && handled++ < 16 && s->state != 3) {
    char *end = NULL;
    for (size_t i = 0; i + 3 < out->length; i++)
      if (!memcmp(out->data + i, "\r\n\r\n", 4)) {
        end = out->data + i;
        break;
      }
    if (!end) {
      if (out->length > 8192)
        s->state = 3;
      return;
    }
    size_t header = (size_t)(end - out->data) + 4, length = 0;
    int found = 0;
    if (header > 8192) {
      s->state = 3;
      return;
    }
    size_t at = 0;
    while (at < header - 2) {
      size_t stop = at;
      while (stop + 1 < header &&
             !(out->data[stop] == '\r' && out->data[stop + 1] == '\n'))
        stop++;
      const char *key = "content-length:";
      size_t k = 0;
      while (k < 15 && at + k < stop &&
             tolower((unsigned char)out->data[at + k]) == key[k])
        k++;
      if (k == 15) {
        if (found++) {
          s->state = 3;
          return;
        }
        size_t p = at + 15;
        while (p < stop && (out->data[p] == ' ' || out->data[p] == '\t'))
          p++;
        size_t first = p;
        for (; p < stop && isdigit((unsigned char)out->data[p]); p++) {
          unsigned digit = (unsigned)(out->data[p] - '0');
          if (length > (MESSAGE_LIMIT - digit) / 10) {
            s->state = 3;
            return;
          }
          length = length * 10 + digit;
        }
        while (p < stop && (out->data[p] == ' ' || out->data[p] == '\t'))
          p++;
        if (first == p || p != stop) {
          s->state = 3;
          return;
        }
      }
      at = stop + 2;
    }
    if (!found || !length) {
      s->state = 3;
      return;
    }
    if (out->length < header + length)
      return;
    message(e, s, out->data + header, length);
    process_consume(out, header + length);
  }
}
static void save_document(struct lsp_document *d) {
  lsp_session *s = d->server;
  if (!s->save || !d->opened)
    return;
  json_buf b = {0};
  json_add(&b, "{\"textDocument\":");
  text_document(&b, d->uri);
  if (s->save_text) {
    json_add(&b, ",\"text\":");
    json_quote_bytes(&b, d->snapshot, d->bytes);
  }
  json_add(&b, "}");
  send_message(s, "textDocument/didSave", 0, &b);
  json_buf_free(&b);
}
int lsp_busy(const editor *e) {
  if (!e->lsp)
    return 0;
  if (e->lsp->request_id)
    return 1;
  for (int i = 0; i < SESSION_LIMIT; i++) {
    const lsp_session *s = &e->lsp->sessions[i];
    if ((s->state == 2 && s->process.in.length) || s->state == 1 ||
        s->state == 4 || s->state == 5)
      return 1;
  }
  return 0;
}
void lsp_tick(editor *e) {
  if (!e->lsp)
    return;
  uint64_t now = adm_milliseconds();
  for (int i = 0; i < SESSION_LIMIT; i++) {
    lsp_session *s = &e->lsp->sessions[i];
    if (!s->language)
      continue;
    if (s->process.running || s->process.out.open || s->process.err.open) {
      int done = process_poll(&s->process);
      receive(e, s);
      if (s->process.err.length)
        process_consume(&s->process.err, s->process.err.length);
      if (done && (s->state == 4 || s->state == 5))
        s->state = 0;
      if ((s->state == 4 || s->state == 5) && now > s->deadline) {
        process_cancel(&s->process);
        s->state = 0;
      }
      if ((done && (s->state == 1 || s->state == 2)) || s->process.failed ||
          (s->state == 1 && now > s->deadline)) {
        s->state = 3;
        snprintf(s->error, sizeof s->error,
                 "%s unavailable or unresponsive; check installation and M-x r",
                 s->language->server_argv[0]);
      }
      if (s->state == 3) {
        process_cancel(&s->process);
        while (s->documents)
          lsp_detach(s->documents->doc);
        if (e->lsp->request_server == s)
          cancel_request(e->lsp);
      }
    }
  }
  for (int i = 0; i < SESSION_LIMIT; i++) {
    lsp_session *s = &e->lsp->sessions[i];
    if (s->restart && s->state == 0 && !s->process.running)
      start_server(e, s);
  }
  if (e->lsp->request_id &&
      (now > e->lsp->request_deadline || !request_current(e))) {
    int timeout = now > e->lsp->request_deadline;
    cancel_request(e->lsp);
    if (timeout)
      snprintf(e->notice, sizeof e->notice, "LSP request timed out");
  }
  for (editor_tab *tab = e->tabs; tab; tab = tab->next) {
    workspace *w = tabs_workspace(e, tab);
    for (int i = 0; i < MAX_PANES; i++) {
      document *doc = w->panes[i].doc;
      const language *lang =
          doc && !doc->readonly ? language_for_filename(doc->filename) : NULL;
      if (!lang)
        continue;
      lsp_session *s = session(e, lang, e->lsp->automatic);
      if (!s)
        continue;
      if (e->lsp->automatic && !s->suppressed && s->state == 0)
        start_server(e, s);
      if (s->state != 2)
        continue;
      struct lsp_document *d = attach(s, doc);
      if (!d)
        continue;
      if (d->generation != doc->change_id && !d->changed_at) {
        d->changed_at = now;
        clear_diagnostics(d);
      }
      if ((!d->opened && !d->paused) ||
          (d->generation != doc->change_id &&
           (now - d->changed_at >= 250 || d->save_pending))) {
        sync_document(d);
        d->changed_at = 0;
      }
      if (d->save_pending && d->generation == doc->change_id) {
        save_document(d);
        d->save_pending = 0;
      }
    }
  }
}
static void definition_opened(editor *e, int result) {
  if (!e->lsp)
    return;
  if (result <= 0 || !e->view->doc) {
    clear_popup(e->lsp);
    return;
  }
  lsp_item *item = &e->lsp->items[e->lsp->selected];
  e->view->cy = item->sr;
  const char *line = buffer_line(&e->view->doc->buf, item->sr);
  e->view->cx = lsp_position_bytes(
      line, item->sc,
      e->lsp->request_server ? e->lsp->request_server->utf8 : 0);
  view_clamp(e->view);
  cursor_mark_column(e);
  dispatch_change(e);
  clear_popup(e->lsp);
}
static void accept(editor *e) {
  struct lsp_state *l = e->lsp;
  if (l->selected < 0 || (size_t)l->selected >= l->count)
    return;
  lsp_item *item = &l->items[l->selected];
  if (!request_current(e)) {
    clear_popup(l);
    snprintf(e->notice, sizeof e->notice, "LSP result is stale; request again");
    return;
  }
  if (l->popup == 2) {
    char *path = lsp_uri_path(item->uri);
    if (!path)
      return;
    l->popup = 0;
    documents_open(e, path, definition_opened);
    free(path);
    return;
  }
  if (item->unsupported) {
    snprintf(e->notice, sizeof e->notice,
             "Completion requires unsupported snippets or extra edits");
    return;
  }
  document *doc = e->view->doc;
  int sr = l->row, sc = l->col, er = sr, ec = sc;
  if (item->range) {
    sr = item->sr;
    er = item->er;
    if (sr >= doc->buf.nlines || er >= doc->buf.nlines)
      return;
    sc = lsp_position_bytes(buffer_line(&doc->buf, sr), item->sc,
                            l->request_server->utf8);
    ec = lsp_position_bytes(buffer_line(&doc->buf, er), item->ec,
                            l->request_server->utf8);
    if (lsp_position_units(buffer_line(&doc->buf, sr), sc,
                           l->request_server->utf8) != item->sc ||
        lsp_position_units(buffer_line(&doc->buf, er), ec,
                           l->request_server->utf8) != item->ec) {
      snprintf(e->notice, sizeof e->notice,
               "Invalid completion range; text unchanged");
      return;
    }
  } else {
    const char *line = buffer_line(&doc->buf, sr);
    while (sc > 0 &&
           (isalnum((unsigned char)line[sc - 1]) || line[sc - 1] == '_' ||
            (unsigned char)line[sc - 1] >= 128))
      sc--;
  }
  buffer_edit edit;
  if (buffer_replace_span(&doc->buf, sr, sc, er, ec, item->text,
                          strlen(item->text), &edit) < 0) {
    snprintf(e->notice, sizeof e->notice,
             "Cannot apply completion; text unchanged");
    return;
  }
  e->view->cy = edit.new_row;
  e->view->cx = edit.new_col;
  selection_clear(e);
  cursor_mark_column(e);
  doc->dirty = 1;
  dispatch_change(e);
  clear_popup(l);
}
static void diagnostic_move(editor *e, int direction) {
  struct lsp_document *d = e->view->doc ? e->view->doc->lsp : NULL;
  if (!d || !d->diagnostic_count || d->generation != d->doc->change_id) {
    snprintf(e->notice, sizeof e->notice, "No current diagnostics");
    return;
  }
  size_t best = 0;
  int found = 0;
  for (size_t i = 0; i < d->diagnostic_count; i++) {
    diagnostic *a = &d->diagnostics[i], *b = &d->diagnostics[best];
    int relative =
        a->row > e->view->cy || (a->row == e->view->cy && a->col > e->view->cx);
    if (direction < 0)
      relative = a->row < e->view->cy ||
                 (a->row == e->view->cy && a->col < e->view->cx);
    int nearer =
        direction > 0
            ? (a->row < b->row || (a->row == b->row && a->col < b->col))
            : (a->row > b->row || (a->row == b->row && a->col > b->col));
    if (relative && (!found || nearer)) {
      best = i;
      found = 1;
    }
  }
  if (!found)
    for (size_t i = 1; i < d->diagnostic_count; i++) {
      diagnostic *a = &d->diagnostics[i], *b = &d->diagnostics[best];
      if (direction > 0
              ? (a->row < b->row || (a->row == b->row && a->col < b->col))
              : (a->row > b->row || (a->row == b->row && a->col > b->col)))
        best = i;
    }
  diagnostic *a = &d->diagnostics[best];
  e->view->cy = a->row;
  e->view->cx = a->col;
  selection_clear(e);
  cursor_mark_column(e);
  snprintf(e->notice, sizeof e->notice, "LSP %s %d:%d: %s",
           a->severity == 1   ? "error"
           : a->severity == 2 ? "warning"
                              : "info",
           a->row + 1, utf8_cols(buffer_line(&d->doc->buf, a->row), a->col) + 1,
           a->message);
}
const char *lsp_status(const editor *e) {
  if (!e->lsp)
    return "Unavailable";
  const language *lang =
      e->view->doc ? language_for_filename(e->view->doc->filename) : NULL;
  if (!lang)
    return "No language for this file";
  lsp_session *s = session((editor *)e, lang, 0);
  if (!s || s->state == 0)
    return "Stopped";
  if (s->state == 1)
    return "Starting";
  if (s->state == 4 || s->state == 5)
    return "Stopping";
  if (s->state == 3)
    return s->error[0] ? s->error : "Protocol error";
  if (e->view->doc->lsp && e->view->doc->lsp->paused)
    return "Paused: file over 1 MiB or invalid UTF-8";
  return "Ready";
}
int lsp_modal(const editor *e) {
  return e->lsp && (e->lsp->menu || e->lsp->popup || e->lsp->name_prompt);
}
static int rename_key(editor *e, int key) {
  struct lsp_state *l = e->lsp;
  if (!l->name_prompt)
    return 0;
  if (key == '\x1b' || key == CTRL('g')) {
    l->name_prompt = 0;
    return 1;
  }
  int at = l->name_cursor;
  if (key == KEY_LEFT)
    l->name_cursor = grapheme_prev(l->new_name, at);
  else if (key == KEY_RIGHT)
    l->name_cursor = grapheme_next(l->new_name, at);
  else if (key == KEY_HOME || key == CTRL('a'))
    l->name_cursor = 0;
  else if (key == KEY_END || key == CTRL('e'))
    l->name_cursor = l->name_length;
  else if (key == KEY_BACKSPACE || key == KEY_DELETE || key == CTRL('d')) {
    int from = key == KEY_BACKSPACE ? grapheme_prev(l->new_name, at) : at;
    int to = key == KEY_BACKSPACE ? at : grapheme_next(l->new_name, at);
    memmove(l->new_name + from, l->new_name + to,
            (size_t)(l->name_length - to) + 1);
    l->name_length -= to - from;
    l->name_cursor = from;
  } else if (key == '\r') {
    if (!l->name_length) {
      snprintf(e->notice, sizeof e->notice, "Enter the new symbol name");
      return 1;
    }
    l->name_prompt = 0;
    request(e, 4);
  } else if (key >= 32 && key < KEY_SPECIAL && key != KEY_BACKSPACE) {
    char seq[4];
    int n = utf8_encode(key, seq);
    if (n && l->name_length + n < (int)sizeof l->new_name) {
      memmove(l->new_name + at + n, l->new_name + at,
              (size_t)(l->name_length - at) + 1);
      memcpy(l->new_name + at, seq, (size_t)n);
      l->name_length += n;
      l->name_cursor += n;
    }
  }
  return 1;
}
static int name_start(const editor *e, int *prefix) {
  *prefix = e->cols >= 30 ? 18 : e->cols >= 12 ? 6 : 2;
  int width = e->cols - *prefix - 1;
  if (width < 0)
    width = 0;
  int at = 0;
  while (at < e->lsp->name_cursor &&
         utf8_cols(e->lsp->new_name, e->lsp->name_cursor) -
                 utf8_cols(e->lsp->new_name, at) >
             width)
    at = grapheme_next(e->lsp->new_name, at);
  return at;
}
int lsp_cursor(const editor *e, int *x, int *y) {
  if (!e->lsp || !e->lsp->name_prompt)
    return 0;
  int prefix, start = name_start(e, &prefix);
  *x = prefix + utf8_cols(e->lsp->new_name, e->lsp->name_cursor) -
       utf8_cols(e->lsp->new_name, start);
  if (*x >= e->cols)
    *x = e->cols - 1;
  if (*x < 0)
    *x = 0;
  *y = e->rows - 1;
  return 1;
}
int lsp_key(editor *e, int key) {
  struct lsp_state *l = e->lsp;
  if (!l)
    return 0;
  if (rename_key(e, key))
    return 1;
  if (key == META('x') && !e->confirmation && !e->new_file.active &&
      !e->help_active) {
    cancel_request(l);
    clear_popup(l);
    l->menu = 1;
    l->scroll = 0;
    e->prefix_active = 0;
    return 1;
  }
  if (l->popup) {
    if (key == '\x1b' || key == CTRL('g')) {
      clear_popup(l);
      return 1;
    }
    if (key == KEY_DOWN) {
      if (l->popup == 1)
        l->scroll++;
      else if ((size_t)(l->selected + 1) < l->count)
        l->selected++;
    } else if (key == KEY_UP) {
      if (l->popup == 1 && l->scroll > 0)
        l->scroll--;
      else if (l->selected > 0)
        l->selected--;
    } else if (key == '\r' && l->popup != 1)
      accept(e);
    return 1;
  }
  if (!l->menu) {
    if ((key == '\x1b' || key == CTRL('g')) && l->request_id) {
      cancel_request(l);
      e->notice[0] = 0;
      return 1;
    }
    return 0;
  }
  if (key == '\x1b' || key == CTRL('g')) {
    l->menu = 0;
    return 1;
  }
  if (key == KEY_DOWN) {
    l->scroll++;
    return 1;
  }
  if (key == KEY_UP) {
    if (l->scroll)
      l->scroll--;
    return 1;
  }
  const language *lang = e->view->doc && !e->view->doc->readonly
                             ? language_for_filename(e->view->doc->filename)
                             : NULL;
  lsp_session *s = session(e, lang, key == 's' || key == 'r');
  if (key == 'a') {
    l->automatic = !l->automatic;
    if (l->automatic)
      for (int i = 0; i < SESSION_LIMIT; i++)
        l->sessions[i].suppressed = 0;
    return 1;
  }
  if (key == 't') {
    e->syntax_enabled = !e->syntax_enabled;
    return 1;
  }
  if (key == '?') {
    clear_popup(l);
    l->hover = copy(
        "LSP: M-x opens this menu. Esc cancels.\ns: start  x: stop  r: "
        "restart\nh: symbol information  d: definition  c: complete\nn/p: "
        "next/previous diagnostic (wrap)\nN: rename symbol  f: format file\na: "
        "automatic server startup  t: "
        "syntax colours\nInstall the server separately; HELP.md lists "
        "commands.\nReplies for changed files or moved cursors are "
        "ignored.\nLarge files remain editable; LSP sync is capped at 1 "
        "MiB.\nCompletion inserts one undoable edit. Extra edits and "
        "snippets\nare not applied. Use Up/Down to select; Enter accepts.");
    l->menu = 0;
    l->popup = 1;
    return 1;
  }
  if (!lang) {
    snprintf(e->notice, sizeof e->notice,
             "LSP supports C, C++, JavaScript, Rust and Python files");
    return 1;
  }
  if (key != 's' && key != 'x' && key != 'r' && key != 'h' && key != 'd' &&
      key != 'c' && key != 'n' && key != 'p' && key != 'N' && key != 'f')
    return 1;
  l->menu = 0;
  e->sidebar.focused = 0;
  if (key == 's')
    start_server(e, s);
  else if (key == 'x')
    stop_server(e, s);
  else if (key == 'r') {
    stop_server(e, s);
    start_server(e, s);
  } else if (key == 'n' || key == 'p')
    diagnostic_move(e, key == 'n' ? 1 : -1);
  else if (key == 'N') {
    l->name_prompt = 1;
    l->name_cursor = l->name_length = 0;
    l->new_name[0] = 0;
  } else
    request(e, key == 'h' ? 1 : key == 'd' ? 2 : key == 'f' ? 5 : 3);
  return 1;
}
static void popup_row(editor *e, abuf *ab, int y, const char *text,
                      int selected) {
  screen_position(ab, 0, y);
  const char *colour = selected ? theme_colour(THEME_ACTIVE) : theme_colour(THEME_POPUP);
  ab_append(ab, colour, (int)strlen(colour));
  int used = screen_text(ab, text, e->cols);
  screen_repeat(ab, ' ', e->cols - used);
  theme_append(ab, THEME_NORMAL);
}
static void draw(editor *e, abuf *ab) {
  struct lsp_state *l = e->lsp;
  if (l && l->name_prompt) {
    int prefix, start = name_start(e, &prefix);
    screen_position(ab, 0, e->rows - 1);
    theme_append(ab, THEME_STATUS);
    const char *title = prefix == 18  ? " RENAME New name: "
                        : prefix == 6 ? "Name: "
                                      : "N:";
    int used = screen_text(ab, title, e->cols);
    used += screen_text(ab, l->new_name + start, e->cols - used - 1);
    screen_repeat(ab, ' ', e->cols - used);
    theme_append(ab, THEME_NORMAL);
    return;
  }
  if (!l || (!l->menu && !l->popup))
    return;
  int height = e->rows > 2 ? e->rows - 2 : 0;
  if (!height || e->cols < 1)
    return;
  popup_row(e, ab, 1,
            l->menu         ? " LSP  M-x  |  Esc: close"
            : l->popup == 1 ? " LSP  Information  |  Esc: close"
            : l->popup == 2 ? " LSP  Definitions  |  Enter: open"
                            : " LSP  Completion  |  Enter: insert",
            1);
  if (l->menu) {
    char status[256];
    const language *lang =
        e->view->doc ? language_for_filename(e->view->doc->filename) : NULL;
    snprintf(status, sizeof status, " %s  |  %s  |  diagnostics: %zu",
             lang ? lang->name : "Plain text", lsp_status(e),
             lsp_diagnostic_count(e->view->doc));
    char automatic[96], syntax[96];
    snprintf(automatic, sizeof automatic, " a  Automatic servers: %s",
             l->automatic ? "on" : "off");
    snprintf(syntax, sizeof syntax, " t  Syntax highlighting: %s",
             e->syntax_enabled ? "on" : "off");
    const char *rows[] = {status,
                          " s  Start server",
                          " x  Stop server",
                          " r  Restart server",
                          " h  Symbol information",
                          " d  Go to definition",
                          " c  Complete at cursor",
                          " N  Rename symbol",
                          " f  Format file",
                          " n / p  Next / previous diagnostic",
                          automatic,
                          syntax,
                          " ?  LSP help"};
    int count = (int)(sizeof rows / sizeof rows[0]), capacity = height - 1;
    if (l->scroll > count - capacity)
      l->scroll = count - capacity > 0 ? count - capacity : 0;
    for (int i = 0; i < capacity; i++)
      popup_row(e, ab, i + 2, i + l->scroll < count ? rows[i + l->scroll] : "",
                0);
  } else if (l->popup == 1) {
    const char *text = l->hover ? l->hover : "";
    for (int i = 0; i < l->scroll && *text; i++) {
      const char *next = strchr(text, '\n');
      text = next ? next + 1 : text + strlen(text);
    }
    for (int y = 2; y <= height; y++) {
      const char *end = strchr(text, '\n');
      size_t len = end ? (size_t)(end - text) : strlen(text);
      char row[4096];
      if (len >= sizeof row)
        len = sizeof row - 1;
      memcpy(row, text, len);
      row[len] = 0;
      popup_row(e, ab, y, row, 0);
      text = end ? end + 1 : text + strlen(text);
    }
  } else {
    int capacity = height - 1;
    if (capacity < 1)
      capacity = 1;
    int first = l->selected >= capacity ? l->selected - capacity + 1 : 0;
    for (int y = 2; y <= height; y++) {
      size_t i = (size_t)(first + y - 2);
      popup_row(e, ab, y, i < l->count ? l->items[i].label : "",
                i < l->count && (int)i == l->selected);
    }
  }
  char bottom[160];
  snprintf(bottom, sizeof bottom, " LSP  %s",
           l->menu         ? "Up/Down: scroll   Esc: close"
           : l->popup == 1 ? "Up/Down: scroll   Esc: close"
                           : "Up/Down: select   Enter: accept   Esc: close");
  popup_row(e, ab, e->rows - 1, bottom, 1);
}
static void init(editor *e) {
  languages_init();
  e->syntax_enabled = 1;
  e->lsp = calloc(1, sizeof *e->lsp);
}
static void shutdown(editor *e) {
  if (!e->lsp)
    return;
  cancel_request(e->lsp);
  clear_popup(e->lsp);
  for (int i = 0; i < SESSION_LIMIT; i++) {
    stop_server(e, &e->lsp->sessions[i]);
    process_dispose(&e->lsp->sessions[i].process);
  }
  free(e->lsp);
  e->lsp = NULL;
}
static module lsp = {.name = "lsp",
                     .init = init,
                     .on_key = lsp_key,
                     .on_draw = draw,
                     .shutdown = shutdown};
module *lsp_module(void) { return &lsp; }
