#include "documents.h"
#include "dispatch.h"
#include "git_panel.h"
#include "path.h"

#include <stdlib.h>
#include <string.h>

static char *copy(const char *text) {
  char *result = malloc(strlen(text) + 1);
  if (result)
    strcpy(result, text);
  return result;
}

static void finish(editor *e, int result) {
  document_completion completion = e->open_request.completion;
  documents_shutdown(e);
  if (completion)
    completion(e, result);
}

void documents_shutdown(editor *e) {
  free(e->open_request.path);
  free(e->open_request.text);
  free(e->open_request.title);
  e->open_request = (document_request){0};
}

static void cancel(editor *e) {
  finish(e, -1);
}

static void open_now(editor *e) {
  document_request *request = &e->open_request;
  document *doc = NULL;
  if (request->path) {
    for (int i = 0; i < MAX_PANES; i++)
      if (document_matches(e->windows.panes[i].doc, request->path)) {
        doc = e->windows.panes[i].doc;
        break;
      }
    if (!doc)
      doc = document_open(request->path);
  } else
    doc = document_preview(request->title, request->text);
  if (doc) {
    e->windows.active = request->pane;
    e->view = &e->windows.panes[request->pane];
    layout_set_document(e, doc);
    layout_arrange(e);
    e->sidebar.focused = 0;
  }
  finish(e, doc != NULL);
}

static void request_open(editor *e) {
  document_request *request = &e->open_request;
  document *current = e->windows.panes[request->pane].doc;
  if (current->dirty && current->views == 1 &&
      (!request->path || !document_matches(current, request->path)))
    dispatch_confirm_with_cancel(e, open_now, cancel, "Replace unsaved buffer?");
  else
    open_now(e);
}

void documents_open(editor *e, const char *path, document_completion completion) {
  documents_shutdown(e);
  e->open_request = (document_request){.path = copy(path), .pane = e->windows.active,
                                       .completion = completion};
  if (!e->open_request.path)
    finish(e, 0);
  else
    request_open(e);
}

void documents_preview(editor *e, int pane, const char *title, const char *text,
                        document_completion completion) {
  documents_shutdown(e);
  e->open_request = (document_request){.title = copy(title), .text = copy(text),
                                       .pane = pane, .completion = completion};
  if (!e->open_request.title || !e->open_request.text)
    finish(e, 0);
  else
    request_open(e);
}

int documents_editable(const editor *e) {
  return !e->view->doc->readonly && !git_panel_worktree_busy(e);
}

static int belongs(const document *doc, const char *root) {
  if (!doc || !doc->filename || !root || !*root)
    return 0;
  char *path = path_absolute(doc->filename);
  if (!path) {
    if (doc->filename[0] == '/' || (strlen(doc->filename) > 1 && doc->filename[1] == ':'))
      path = copy(doc->filename);
    else {
      char *cwd = path_current_directory();
      if (cwd) {
        path = path_join(cwd, doc->filename);
        free(cwd);
      }
    }
  }
  size_t length = strlen(root);
  int match = path &&
#ifdef _WIN32
      !_strnicmp(path, root, length) &&
#else
      !strncmp(path, root, length) &&
#endif
      (root[length - 1] == '/' || path[length] == '/' || path[length] == '\\');
  free(path);
  return match;
}

int documents_unsaved_in(const editor *e, const char *root) {
  for (int i = 0; i < MAX_PANES; i++) {
    document *doc = e->windows.panes[i].doc;
    if (doc && doc->dirty && belongs(doc, root))
      return 1;
  }
  return 0;
}

// Worktree operations reload each clean document once, retaining its shared views.
void documents_reload(editor *e, const char *root) {
  for (int i = 0; i < MAX_PANES; i++) {
    document *doc = e->windows.panes[i].doc;
    if (!doc || doc->dirty || !belongs(doc, root))
      continue;
    int seen = 0;
    for (int j = 0; j < i; j++)
      seen |= e->windows.panes[j].doc == doc;
    if (seen)
      continue;
    buffer next;
    if (load(&next, doc->filename) < 0)
      continue;
    next.on_edit = doc->buf.on_edit;
    next.edit_context = doc->buf.edit_context;
    buffer_free(&doc->buf);
    doc->buf = next;
    for (int j = 0; j < MAX_PANES; j++) {
      view *v = &e->windows.panes[j];
      if (v->doc == doc) {
        v->sel_active = v->sel_mode = 0;
        view_clamp(v);
      }
    }
  }
}
