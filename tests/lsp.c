#include "lsp.h"
#include "fileio.h"
#include "json.h"
#include "path.h"
#include "screen.h"
#include "shared_views.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
static void pause_tick(void) { Sleep(1); }
#else
#include <unistd.h>
static void pause_tick(void) { usleep(1000); }
#endif
static void emit(const char *body) {
  char header[128];
  int n = snprintf(header, sizeof header,
                   "Content-Length: %zu\r\nContent-Type: "
                   "application/vscode-jsonrpc; charset=utf-8\r\n\r\n",
                   strlen(body));
  fwrite(header, 1, 7, stdout);
  fflush(stdout);
  pause_tick();
  fwrite(header + 7, 1, (size_t)n - 7, stdout);
  fputs(body, stdout);
  fflush(stdout);
}
static void response(int64_t id, const char *result) {
  json_buf b = {0};
  json_add(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
  json_number(&b, id);
  json_add(&b, ",\"result\":");
  json_add(&b, result);
  json_add(&b, "}");
  emit(b.data);
  json_buf_free(&b);
}
static void log_method(const char *method) {
  FILE *f = fopen(getenv("ADM_LSP_TEST_LOG"), "ab");
  assert(f);
  fprintf(f, "%s\n", method);
  fclose(f);
}
static size_t byte_position(const char *text, int row, int units) {
  size_t at = 0;
  while (row-- && text[at]) {
    while (text[at] && text[at] != '\n')
      at++;
    if (text[at])
      at++;
  }
  return at + (size_t)lsp_position_bytes(text + at, units, 0);
}
static void publish(const char *uri, int version) {
  json_buf b = {0};
  json_add(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/"
               "publishDiagnostics\",\"params\":{\"uri\":");
  json_quote(&b, uri);
  json_add(&b, ",\"version\":");
  json_number(&b, version);
  json_add(&b, ",\"diagnostics\":[{\"range\":{\"start\":{\"line\":0,"
               "\"character\":0},\"end\":{\"line\":0,\"character\":1}},"
               "\"severity\":2,\"message\":\"Test warning\"}]}}");
  emit(b.data);
  json_buf_free(&b);
}
static int server(void) {
  struct fake_doc {
    char *uri, *text;
    int version;
  } docs[16] = {{0}};
  int shutdown = 0;
  for (;;) {
    char header[256];
    size_t bytes = 0;
    while (fgets(header, sizeof header, stdin)) {
      if (!strcmp(header, "\r\n"))
        break;
      if (sscanf(header, "Content-Length: %zu", &bytes) == 1)
        continue;
    }
    if (feof(stdin))
      break;
    assert(bytes && bytes < 8u * 1024u * 1024u);
    char *body = malloc(bytes + 1);
    assert(body && fread(body, 1, bytes, stdin) == bytes);
    body[bytes] = 0;
    json_value v = {0};
    assert(json_parse(&v, body, bytes));
    char *method = json_string(&v, json_get(&v, 0, "method"));
    int64_t id = 0;
    json_int(&v, json_get(&v, 0, "id"), &id);
    if (!method) {
      assert(id == 10000);
      json_free(&v);
      free(body);
      continue;
    }
    log_method(method);
    size_t params = json_get(&v, 0, "params"),
           doc = json_get(&v, params, "textDocument");
    char *incoming = json_string(&v, json_get(&v, doc, "uri"));
    int slot = -1;
    if (incoming) {
      for (int i = 0; i < 16; i++)
        if (docs[i].uri && !strcmp(docs[i].uri, incoming))
          slot = i;
      if (slot < 0)
        for (int i = 0; i < 16; i++)
          if (!docs[i].uri) {
            slot = i;
            break;
          }
    }
    free(incoming);
    char *uri = slot >= 0 ? docs[slot].uri : NULL,
         *text = slot >= 0 ? docs[slot].text : NULL;
    int version = slot >= 0 ? docs[slot].version : 0;

    if (!strcmp(method, "initialize")) {
      char *root = json_string(&v, json_get(&v, params, "rootUri"));
      assert(root && strstr(root, "file://"));
      free(root);
      response(
          id, "{\"capabilities\":{\"textDocumentSync\":{\"openClose\":true,"
              "\"change\":2,\"save\":{\"includeText\":true}},\"hoverProvider\":"
              "true,\"definitionProvider\":true,\"completionProvider\":{},"
              "\"renameProvider\":true,\"documentFormattingProvider\":true}}");
      emit("{\"jsonrpc\":\"2.0\",\"id\":10000,\"method\":\"workspace/"
           "configuration\",\"params\":{\"items\":[{},{}]}}");
    } else if (!strcmp(method, "textDocument/didOpen")) {
      assert(!uri);
      uri = json_string(&v, json_get(&v, doc, "uri"));
      text = json_string(&v, json_get(&v, doc, "text"));
      assert(uri && text);
      int64_t n;
      assert(json_int(&v, json_get(&v, doc, "version"), &n));
      version = (int)n;
      publish(uri, version);
    } else if (!strcmp(method, "textDocument/didChange")) {
      assert(uri && text);
      size_t changes = json_get(&v, params, "contentChanges");
      for (size_t i = json_first(&v, changes);
           i != JSON_NONE && i < v.tokens[changes].next; i = v.tokens[i].next) {
        char *replacement = json_string(&v, json_get(&v, i, "text"));
        assert(replacement);
        size_t r = json_get(&v, i, "range"), a = json_get(&v, r, "start"),
               b = json_get(&v, r, "end");
        int64_t ar, ac, br, bc;
        assert(json_int(&v, json_get(&v, a, "line"), &ar) &&
               json_int(&v, json_get(&v, a, "character"), &ac) &&
               json_int(&v, json_get(&v, b, "line"), &br) &&
               json_int(&v, json_get(&v, b, "character"), &bc));
        size_t start = byte_position(text, (int)ar, (int)ac),
               end = byte_position(text, (int)br, (int)bc),
               n = strlen(replacement), old = strlen(text);
        assert(start <= end && end <= old);
        char *next = malloc(old - (end - start) + n + 1);
        assert(next);
        memcpy(next, text, start);
        memcpy(next + start, replacement, n);
        memcpy(next + start + n, text + end, old - end + 1);
        free(text);
        text = next;
        free(replacement);
      }
      int64_t n;
      assert(json_int(&v, json_get(&v, doc, "version"), &n) && n > version);
      version = (int)n;
      publish(uri, version);
    } else if (!strcmp(method, "textDocument/didSave")) {
      char *saved = json_string(&v, json_get(&v, params, "text"));
      assert(saved && text && !strcmp(saved, text));
      free(saved);
    } else if (!strcmp(method, "textDocument/hover"))
      response(id, "{\"contents\":{\"kind\":\"markdown\",\"value\":\"Symbol "
                   "information\\nSecond line\"}}");
    else if (!strcmp(method, "textDocument/completion"))
      response(id, "[{\"label\":\"print\",\"textEdit\":{\"range\":{\"start\":{"
                   "\"line\":0,\"character\":0},\"end\":{\"line\":0,"
                   "\"character\":3}},\"newText\":\"print\"}}]");
    else if (!strcmp(method, "textDocument/definition")) {
      json_buf b = {0};
      json_add(&b, "[{\"targetUri\":");
      json_quote(&b, uri);
      json_add(&b, ",\"targetSelectionRange\":{\"start\":{\"line\":1,"
                   "\"character\":0},\"end\":{\"line\":1,\"character\":1}}}]");
      response(id, b.data);
      json_buf_free(&b);
    } else if (!strcmp(method, "textDocument/formatting")) {
      response(id, "[{\"range\":{\"start\":{\"line\":0,\"character\":0},"
                   "\"end\":{\"line\":0,\"character\":0}},\"newText\":\" "
                   "\"},{\"range\":{\"start\":{\"line\":0,\"character\":5},"
                   "\"end\":{\"line\":0,\"character\":5}},\"newText\":\" \"}]");
    } else if (!strcmp(method, "textDocument/rename")) {
      char *name = json_string(&v, json_get(&v, params, "newName")),
           *target = lsp_uri(getenv("ADM_LSP_RENAME_TARGET"));
      assert(name && target);
      json_buf b = {0};
      json_add(&b, "{\"documentChanges\":[{\"textDocument\":{\"uri\":");
      json_quote(&b, uri);
      json_add(&b, ",\"version\":");
      json_number(&b, version);
      json_add(&b,
               "},\"edits\":[{\"range\":{\"start\":{\"line\":0,\"character\":0}"
               ",\"end\":{\"line\":0,\"character\":5}},\"newText\":");
      json_quote(&b, name);
      json_add(&b, "}]},{\"textDocument\":{\"uri\":");
      json_quote(&b, target);
      json_add(&b, ",\"version\":null},\"edits\":[{\"range\":{\"start\":{"
                   "\"line\":0,\"character\":0},\"end\":{\"line\":0,"
                   "\"character\":5}},\"newText\":");
      json_quote(&b, name);
      json_add(&b, "}]}]}");
      response(id, b.data);
      json_buf_free(&b);
      free(name);
      free(target);
    } else if (!strcmp(method, "textDocument/didClose")) {
      free(uri);
      free(text);
      uri = text = NULL;
    } else if (!strcmp(method, "shutdown")) {
      shutdown = 1;
      response(id, "null");
    } else if (!strcmp(method, "exit")) {
      assert(shutdown);
      json_free(&v);
      free(body);
      free(method);
      for (int i = 0; i < 16; i++) {
        free(docs[i].uri);
        free(docs[i].text);
      }
      return 0;
    }
    if (slot >= 0) {
      docs[slot].uri = uri;
      docs[slot].text = text;
      docs[slot].version = version;
    }
    json_free(&v);
    free(body);
    free(method);
  }
  for (int i = 0; i < 16; i++) {
    free(docs[i].uri);
    free(docs[i].text);
  }
  return 0;
}
static void menu(editor *e, int key) {
  dispatch_key(e, META('x'));
  assert(lsp_modal(e));
  dispatch_key(e, key);
}
static void settle(editor *e) {
  for (int i = 0; i < 350; i++) {
    dispatch_tick(e);
    pause_tick();
  }
}
static void ready(editor *e) {
  for (int i = 0; i < 10000 && strcmp(lsp_status(e), "Ready"); i++) {
    dispatch_tick(e);
    pause_tick();
  }
  assert(!strcmp(lsp_status(e), "Ready"));
  settle(e);
}
static void popup(editor *e) {
  for (int i = 0; i < 10000 && !lsp_modal(e); i++) {
    dispatch_tick(e);
    pause_tick();
  }
  assert(lsp_modal(e));
}
static int logged(const char *log, const char *method) {
  FILE *f = fopen(log, "rb");
  if (!f)
    return 0;
  char line[256];
  int count = 0;
  while (fgets(line, sizeof line, f))
    if (!strncmp(line, method, strlen(method)) && line[strlen(method)] == '\n')
      count++;
  fclose(f);
  return count;
}
int main(int argc, char **argv) {
  if (getenv("ADM_LSP_FAKE_CHILD")) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    return server();
  }
  assert(argc == 2);
  char *filename = malloc(strlen(argv[1]) + 20),
       *log = malloc(strlen(argv[1]) + 8);
  assert(filename && log);
  sprintf(filename, "%s source è.c", argv[1]);
  sprintf(log, "%s.log", argv[1]);
#ifdef _WIN32
  _putenv_s("ADM_LSP_CLANGD", argv[0]);
  _putenv_s("ADM_LSP_TEST_LOG", log);
  _putenv_s("ADM_LSP_FAKE_CHILD", "1");
#else
  assert(setenv("ADM_LSP_CLANGD", argv[0], 1) == 0);
  assert(setenv("ADM_LSP_TEST_LOG", log, 1) == 0);
  assert(setenv("ADM_LSP_FAKE_CHILD", "1", 1) == 0);
#endif
  char *target = malloc(strlen(argv[1]) + 12);
  assert(target);
  sprintf(target, "%s-other.c", argv[1]);
  assert(file_write(target, "print\n") == 0);
#ifdef _WIN32
  _putenv_s("ADM_LSP_RENAME_TARGET", target);
#else
  assert(setenv("ADM_LSP_RENAME_TARGET", target, 1) == 0);
#endif
  assert(file_write(filename, "pri\nsecond") == 0);
  editor e = {.document.filename = filename, .rows = 24, .cols = 80};
  assert(load(&e.document.buf, filename) == 0);
  dispatch_init(&e);
  e.view->cx = 3;
  dispatch_key(&e, META('x'));
  abuf ab = {0};
  dispatch_draw(&e, &ab);
  ab_append(&ab, "\0", 1);
  assert(strstr(ab.b, "LSP") && strstr(ab.b, "Start server"));
  ab_free(&ab);
  dispatch_key(&e, 's');
  ready(&e);
  assert(lsp_diagnostic_count(&e.document) == 1 &&
         logged(log, "textDocument/didOpen") == 1);
  assert(test_shared_split(&e, LAYOUT_VERTICAL));
  assert(test_shared_tab(&e));
  settle(&e);
  assert(logged(log, "textDocument/didOpen") == 1);
  menu(&e, 'h');
  popup(&e);
  dispatch_draw(&e, &ab);
  ab_append(&ab, "\0", 1);
  assert(strstr(ab.b, "Symbol information"));
  ab_free(&ab);
  dispatch_key(&e, '\x1b');
  menu(&e, 'c');
  popup(&e);
  dispatch_key(&e, '\r');
  assert(!lsp_modal(&e));
  assert(!strcmp(buffer_line(&e.document.buf, 0), "print"));
  dispatch_key(&e, CTRL('z'));
  assert(!strcmp(buffer_line(&e.document.buf, 0), "pri"));
  dispatch_key(&e, META('z'));
  assert(!strcmp(buffer_line(&e.document.buf, 0), "print"));
  settle(&e);
  dispatch_key(&e, CTRL('x'));
  dispatch_key(&e, CTRL('s'));
  settle(&e);
  assert(logged(log, "textDocument/didSave") == 1);
  // Definitions use the normal document-opening flow and LSP position units.
  menu(&e, 'd');
  popup(&e);
  dispatch_key(&e, '\r');
  assert(e.view->cy == 1 && e.view->cx == 0 && !lsp_modal(&e));
  // A reply after an edit must not apply to the new cursor/document.
  menu(&e, 'h');
  dispatch_key(&e, 'X');
  settle(&e);
  assert(!lsp_modal(&e) && !strcmp(buffer_line(&e.document.buf, 1), "Xsecond"));
  menu(&e, 'n');
  assert(e.view->cy == 0 && strstr(e.notice, "Test warning"));
  menu(&e, 'f');
  for (int i = 0;
       i < 10000 && strcmp(buffer_line(&e.document.buf, 0), " print "); i++) {
    dispatch_tick(&e);
    pause_tick();
  }
  assert(!strcmp(buffer_line(&e.document.buf, 0), " print "));
  dispatch_key(&e, CTRL('z'));
  assert(!strcmp(buffer_line(&e.document.buf, 0), "print"));
  int before_tabs = e.tab_count;
  menu(&e, 'N');
  int x, y;
  assert(lsp_cursor(&e, &x, &y));
  for (const char *p = "renamed"; *p; p++)
    dispatch_key(&e, *p);
  dispatch_key(&e, '\r');
  for (int i = 0;
       i < 10000 && strcmp(buffer_line(&e.document.buf, 0), "renamed"); i++) {
    dispatch_tick(&e);
    pause_tick();
  }
  assert(!strcmp(buffer_line(&e.document.buf, 0), "renamed") &&
         e.document.dirty && e.tab_count == before_tabs + 1);
  document *other = documents_find(&e, target);
  assert(other && other->dirty && other->views == 1 &&
         !strcmp(buffer_line(&other->buf, 0), "renamed"));
  char *disk = file_read(target);
  assert(disk && !strcmp(disk, "print\n"));
  free(disk);
  dispatch_key(&e, CTRL('z'));
  assert(!strcmp(buffer_line(&e.document.buf, 0), "print"));
  menu(&e, 't');
  assert(!e.syntax_enabled);
  dispatch_key(&e, 't');
  assert(e.syntax_enabled);
  dispatch_key(&e, '\x1b');
  menu(&e, 'x');
  for (int i = 0; i < 3000 && strcmp(lsp_status(&e), "Stopped"); i++) {
    dispatch_tick(&e);
    pause_tick();
  }
  assert(!strcmp(lsp_status(&e), "Stopped"));
  settle(&e);
  assert(logged(log, "textDocument/didClose") == 2 &&
         logged(log, "shutdown") == 1 && logged(log, "exit") == 1 &&
         !e.document.lsp);
  menu(&e, 's');
  ready(&e);
  assert(logged(log, "textDocument/didOpen") == 4);
  // A filesystem rename retains the shared buffer and history, but closes the
  // old LSP URI and opens the new one with current, potentially unsaved text.
  char *renamed = malloc(strlen(filename) + 16);
  assert(renamed);
  sprintf(renamed, "%s.renamed.c", filename);
  char *source_path = path_absolute(e.document.filename);
  char *renamed_path = path_absolute(renamed);
  assert(source_path && renamed_path);
  void *history_before = e.document.history;
  int views_before = e.document.views, dirty_before = e.document.dirty;
  int closes_before = logged(log, "textDocument/didClose");
  assert(!documents_rename_file(&e, source_path, renamed_path));
  assert(!e.document.lsp && e.document.history == history_before &&
         e.document.views == views_before && e.document.dirty == dirty_before);
  settle(&e);
  assert(e.document.lsp && logged(log, "textDocument/didOpen") == 5 &&
         logged(log, "textDocument/didClose") == closes_before + 1);
  free(source_path);
  free(renamed_path);
  free(renamed);
  // Oversized buffers never enqueue enormous didChange notifications.
  char *huge = malloc(1024u * 1024u + 2);
  assert(huge);
  memset(huge, 'x', 1024u * 1024u + 1);
  huge[1024u * 1024u + 1] = 0;
  assert(buffer_replace_span(&e.document.buf, 0, 0, e.document.buf.nlines - 1,
                             (int)strlen(buffer_line(
                                 &e.document.buf, e.document.buf.nlines - 1)),
                             huge, strlen(huge), NULL) == 0);
  dispatch_change(&e);
  free(huge);
  menu(&e, 'h');
  assert(strstr(e.notice, "exceeds 1 MiB"));
  assert(strstr(lsp_status(&e), "Paused"));
  dispatch_key(&e, CTRL('z'));
  menu(&e, 'h');
  popup(&e);
  dispatch_key(&e, '\x1b');
  dispatch_shutdown(&e);
  assert(!e.document.lsp && !e.lsp);
  free(target);
  free(filename);
  free(log);
  puts("LSP regressions passed.");
  return 0;
}
