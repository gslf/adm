#include "documents.h"
#include "undo.h"
#include "syntax.h"
#include "dispatch.h"
#include "git_panel.h"
#include "path.h"
#include "fileio.h"
#include "lsp.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

document *documents_find(const editor *e, const char *path) {
  for (editor_tab *tab = e->tabs; tab; tab = tab->next) {
    workspace *w = tabs_workspace(e, tab);
    for (int i = 0; i < MAX_PANES; i++)
      if (document_matches(w->panes[i].doc, path))
        return w->panes[i].doc;
  }
  return NULL;
}

document *documents_at_path(const editor *e, const char *path) {
  errno = 0;
  for (editor_tab *tab = e->tabs; tab; tab = tab->next) {
    workspace *w = tabs_workspace(e, tab);
    for (int i = 0; i < MAX_PANES; i++) {
      document *doc = w->panes[i].doc;
      if (!doc || !doc->filename)
        continue;
#ifdef _WIN32
      if (!_stricmp(doc->filename, path))
#else
      if (!strcmp(doc->filename, path))
#endif
        return doc;
#ifndef _WIN32
      // Production filenames are canonical absolute paths. Do not resolve
      // unrelated files whose parent might have been removed externally.
      if (doc->filename[0] == '/')
        continue;
#endif
      char *saved_path = path_absolute(doc->filename);
      if (!saved_path)
        return NULL;
      int match = saved_path &&
#ifdef _WIN32
          !_stricmp(saved_path, path);
#else
          !strcmp(saved_path, path);
#endif
      free(saved_path);
      errno = 0;
      if (match)
        return doc;
    }
  }
  return NULL;
}

int documents_rename_file(editor *e, const char *source, const char *destination) {
  if (git_panel_worktree_busy(e)) {
    errno = EBUSY;
    return -1;
  }
  if (!strcmp(source, destination))
    return file_rename(source, destination);
  document *doc = documents_at_path(e, source);
  if (!doc && errno)
    return -1;
  document *target = documents_at_path(e, destination);
  if (!target && errno)
    return -1;
  if (target && target != doc) {
    errno = EEXIST;
    return -1;
  }
  // Allocate before touching disk, so no allocation failure can leave the
  // shared document saving to its old pathname after a successful rename.
  char *filename = doc ? copy(destination) : NULL;
  if (doc && !filename) {
    errno = ENOMEM;
    return -1;
  }
  if (file_rename(source, destination) < 0) {
    free(filename);
    return -1;
  }
  if (doc) {
    lsp_detach(doc);
    syntax_dispose(doc);
    if (doc->allocated || doc->owns_filename)
      free((char *)doc->filename);
    doc->filename = filename;
    doc->owns_filename = 1;
    doc->change_id++;
  }
  return 0;
}

int documents_delete_file(editor *e, const char *path) {
  if (git_panel_worktree_busy(e)) {
    errno = EBUSY;
    return -1;
  }
  document *doc = documents_at_path(e, path);
  if (!doc && errno)
    return -1;
  struct stat info;
#ifdef _WIN32
  if (stat(path, &info) == 0 && (info.st_mode & _S_IFMT) == _S_IFDIR) {
#else
  if (lstat(path, &info) == 0 && S_ISDIR(info.st_mode)) {
#endif
    errno = EISDIR;
    return -1;
  }
  if (remove(path) < 0)
    return -1;
  if (doc) {
    // The explorer has already confirmed discarding any unsaved content.
    // Empty every referencing split, so Save cannot recreate the deleted file.
    document_retain(doc);
    for (editor_tab *tab = e->tabs; tab; tab = tab->next) {
      workspace *w = tabs_workspace(e, tab);
      for (int i = 0; i < MAX_PANES; i++)
        if (w->panes[i].doc == doc) {
          view_bind_document(&w->panes[i], NULL);
          w->panes[i].revision = ++e->next_view_revision;
        }
    }
    document_release(doc);
    layout_arrange(e);
  }
  return 0;
}

static view *request_view(editor *e) {
  document_request *request = &e->open_request;
  editor_tab *tab = tabs_find(e, request->tab_id);
  if (!tab || request->pane < 0 || request->pane >= MAX_PANES)
    return NULL;
  view *v = &tabs_workspace(e, tab)->panes[request->pane];
  return v->used && v->revision == request->revision ? v : NULL;
}

static void open_now(editor *e) {
  document_request *request = &e->open_request;
  view *v = request_view(e);
  if (!v) {
    finish(e, 0);
    return;
  }
  document *doc = NULL;
  if (request->path) {
    struct stat info;
    if (stat(request->path, &info) < 0 ||
#ifdef _WIN32
        (info.st_mode & _S_IFMT) != _S_IFREG
#else
        !S_ISREG(info.st_mode)
#endif
        ) {
      finish(e, 0);
      return;
    }
    doc = documents_find(e, request->path);
    if (!doc)
      doc = document_open(request->path);
  } else
    doc = document_preview(request->title, request->text);
  if (doc) {
    layout_bind_view(e, v, doc);
    if (request->tab_id == e->active_tab->id) {
      e->windows.active = request->pane;
      e->view = v;
      layout_arrange(e);
      e->sidebar.focused = 0;
    }
  }
  finish(e, doc != NULL);
}

static void request_open(editor *e) {
  document_request *request = &e->open_request;
  view *v = request_view(e);
  if (!v) {
    finish(e, 0);
    return;
  }
  document *current = v->doc;
  if (current && current->dirty && current->views == 1 &&
      (!request->path || !document_matches(current, request->path)))
    dispatch_confirm_with_cancel(e, open_now, cancel, "Replace unsaved buffer?");
  else
    open_now(e);
}

void documents_open(editor *e, const char *path, document_completion completion) {
  if (e->confirmation || e->new_file.active) {
    if (completion)
      completion(e, 0);
    return;
  }
  documents_shutdown(e);
  e->open_request = (document_request){.path = copy(path), .pane = e->windows.active,
      .tab_id = e->active_tab->id, .revision = e->view->revision, .completion = completion};
  if (!e->open_request.path)
    finish(e, 0);
  else
    request_open(e);
}

void documents_preview_at(editor *e, unsigned long tab_id, int pane,
                          const char *title, const char *text, document_completion completion) {
  editor_tab *tab = tabs_find(e, tab_id);
  if (e->confirmation || e->new_file.active || !tab || pane < 0 || pane >= MAX_PANES ||
      !tabs_workspace(e, tab)->panes[pane].used) {
    if (completion)
      completion(e, 0);
    return;
  }
  documents_shutdown(e);
  e->open_request = (document_request){.title = copy(title), .text = copy(text),
      .pane = pane, .tab_id = tab_id, .revision = tabs_workspace(e, tab)->panes[pane].revision,
      .completion = completion};
  if (!e->open_request.title || !e->open_request.text)
    finish(e, 0);
  else
    request_open(e);
}

void documents_preview(editor *e, int pane, const char *title, const char *text,
                        document_completion completion) {
  documents_preview_at(e, e->active_tab->id, pane, title, text, completion);
}

int documents_editable(const editor *e) {
  return e->view->doc && !e->view->doc->readonly && !git_panel_worktree_busy(e);
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
  for (editor_tab *tab = e->tabs; tab; tab = tab->next) {
    workspace *w = tabs_workspace(e, tab);
    for (int i = 0; i < MAX_PANES; i++) {
      document *doc = w->panes[i].doc;
      if (doc && doc->dirty && belongs(doc, root))
        return 1;
    }
  }
  return 0;
}

// Worktree operations reload each clean document once, retaining all its views.
void documents_reload(editor *e, const char *root) {
  for (editor_tab *tab = e->tabs; tab; tab = tab->next) {
    workspace *w = tabs_workspace(e, tab);
    for (int i = 0; i < MAX_PANES; i++) {
      document *doc = w->panes[i].doc;
      if (!doc || doc->dirty || !belongs(doc, root))
        continue;
      int seen = 0;
      for (editor_tab *prior = e->tabs; prior; prior = prior->next) {
        workspace *pw = tabs_workspace(e, prior);
        int end = prior == tab ? i : MAX_PANES;
        for (int j = 0; j < end; j++)
          seen |= pw->panes[j].doc == doc;
        if (prior == tab)
          break;
      }
      if (seen)
        continue;
      buffer next;
      if (load(&next, doc->filename) < 0)
        continue;
      next.on_edit = doc->buf.on_edit;
      next.edit_context = doc->buf.edit_context;
      syntax_dispose(doc);
      doc->change_id++;
      undo_clear(doc);
      buffer_free(&doc->buf);
      doc->buf = next;
      undo_attach(doc);
      for (editor_tab *other = e->tabs; other; other = other->next) {
        workspace *ow = tabs_workspace(e, other);
        for (int j = 0; j < MAX_PANES; j++) {
          view *v = &ow->panes[j];
          if (v->doc == doc) {
            v->sel_active = v->sel_mode = 0;
            view_clamp(v);
          }
        }
      }
    }
  }
}
