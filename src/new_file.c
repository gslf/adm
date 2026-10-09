#include "theme.h"
#include "new_file.h"
#include "dispatch.h"
#include "screen.h"
#include "path.h"
#include "utf8.h"
#include "fileio.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void new_file_shutdown(editor *e) {
  free(e->new_file.directory);
  free(e->new_file.rename_path);
  e->new_file = (new_file_prompt){0};
}

static int empty(const editor *e) { return !e->view->doc; }

static char *directory(editor *e) {
  if (e->sidebar.kind != SIDEBAR_FILES)
    sidebar_show(e, SIDEBAR_FILES);
  const tree_entry *entry = file_tree_selected(&e->files.tree);
  if (!entry)
    return path_current_directory();
  size_t length = entry->directory ? strlen(entry->path) :
                  (size_t)(path_name(entry->path) - entry->path);
  char *result = malloc(length + 1);
  if (result) {
    memcpy(result, entry->path, length);
    result[length] = '\0';
  }
  return result;
}

static void begin(editor *e) {
  if (e->view->doc || e->confirmation)
    return;
  new_file_shutdown(e);
  new_file_prompt *p = &e->new_file;
  p->directory = directory(e);
  if (!p->directory) {
    snprintf(e->notice, sizeof e->notice, "Cannot find creation directory");
    return;
  }
  p->active = 1;
  p->tab_id = e->active_tab->id;
  p->pane = e->windows.active;
  p->revision = e->view->revision;
  e->sidebar.focused = 0;
}

void new_file_rename(editor *e, const char *path) {
  if (e->confirmation || e->new_file.active || git_panel_worktree_busy(e)) {
    snprintf(e->files.tree.error, sizeof e->files.tree.error, "Wait for the current operation");
    return;
  }
  const char *name = path_name(path);
  if (strlen(name) >= sizeof e->new_file.name) {
    snprintf(e->files.tree.error, sizeof e->files.tree.error, "File name too long");
    return;
  }
  new_file_shutdown(e);
  new_file_prompt *p = &e->new_file;
  size_t parent = (size_t)(name - path);
  p->directory = malloc(parent + 1);
  p->rename_path = malloc(strlen(path) + 1);
  if (!p->directory || !p->rename_path) {
    new_file_shutdown(e);
    snprintf(e->files.tree.error, sizeof e->files.tree.error, "Out of memory");
    return;
  }
  memcpy(p->directory, path, parent);
  p->directory[parent] = 0;
  strcpy(p->rename_path, path);
  strcpy(p->name, name);
  p->length = p->cursor = (int)strlen(name);
  p->active = 1;
}

static void rename_file(editor *e) {
  new_file_prompt *p = &e->new_file;
  if (!p->length || !strcmp(p->name, ".") || !strcmp(p->name, "..") ||
      strchr(p->name, '/')
#ifdef _WIN32
      || strpbrk(p->name, "\\:<>\"|?*") ||
      p->name[p->length - 1] == '.' || p->name[p->length - 1] == ' '
#endif
      ) {
    snprintf(p->error, sizeof p->error, "Enter a simple file name");
    return;
  }
  char *target = path_join(p->directory, p->name);
  if (!target) {
    snprintf(p->error, sizeof p->error, "Out of memory");
    return;
  }
  if (documents_rename_file(e, p->rename_path, target) < 0) {
    snprintf(p->error, sizeof p->error, "Cannot rename file: %s", strerror(errno));
    free(target);
    return;
  }
  new_file_shutdown(e);
  file_manager_refresh_select(e, target);
  free(target);
  e->sidebar.focused = file_manager_area(e).width > 0;
}

void new_file_bindings(void) {
  dispatch_bind_prefix_when('N', begin, "N", "Create new file", empty);
}

static int absolute(const char *path) {
#ifdef _WIN32
  return (path[0] && path[1] == ':' && (path[2] == '/' || path[2] == '\\')) ||
         (path[0] == '\\' && path[1] == '\\');
#else
  return path[0] == '/';
#endif
}

static void create(editor *e) {
  new_file_prompt *p = &e->new_file;
  if (!p->length) {
    snprintf(p->error, sizeof p->error, "Enter a file name");
    return;
  }
  editor_tab *tab = tabs_find(e, p->tab_id);
  workspace *w = tab ? tabs_workspace(e, tab) : NULL;
  view *v = w && p->pane >= 0 && p->pane < MAX_PANES ? &w->panes[p->pane] : NULL;
  if (!v || !v->used || v->doc || v->revision != p->revision) {
    new_file_shutdown(e);
    snprintf(e->notice, sizeof e->notice, "New-file target changed; try again");
    return;
  }
  int has_path = strchr(p->name, '/') != NULL;
#ifdef _WIN32
  has_path |= strchr(p->name, '\\') != NULL || strchr(p->name, ':') != NULL;
#endif
  if (has_path && !absolute(p->name)) {
    snprintf(p->error, sizeof p->error, "Use an absolute path, or a simple file name");
    return;
  }
  char *joined = has_path ? NULL : path_join(p->directory, p->name);
  char *path = has_path ? path_absolute(p->name) : joined ? path_absolute(joined) : NULL;
  free(joined);
  document *doc = path ? calloc(1, sizeof *doc) : NULL;
  if (!doc) {
    free(path);
    snprintf(p->error, sizeof p->error, "Cannot resolve path or allocate file buffer");
    return;
  }
  // Exclusive creation protects existing files, including symlink targets.
  FILE *fp = file_create(path);
  if (!fp) {
    if (errno == EEXIST)
      snprintf(p->error, sizeof p->error, "File already exists; select it in Explorer");
    else
      snprintf(p->error, sizeof p->error, "Cannot create file: %s", strerror(errno));
    free(path);
    free(doc);
    return;
  }
  if (fclose(fp) != 0) {
    remove(path);
    free(path);
    free(doc);
    snprintf(p->error, sizeof p->error, "Cannot finish creating file");
    return;
  }
  doc->filename = path;
  doc->allocated = 1;
  layout_bind_view(e, v, doc);
  new_file_shutdown(e);
  file_manager_refresh(e);
  e->sidebar.focused = 0;
}

int new_file_key(editor *e, int key) {
  new_file_prompt *p = &e->new_file;
  if (!p->active)
    return 0;
  p->error[0] = '\0';
  if (key == CTRL('g') || key == '\x1b') {
    new_file_shutdown(e);
    sidebar_show(e, SIDEBAR_FILES);
  } else if (key == '\r' || key == '\n') {
    if (p->rename_path)
      rename_file(e);
    else
      create(e);
  } else if (key == KEY_LEFT || key == CTRL('b'))
    p->cursor = grapheme_prev(p->name, p->cursor);
  else if (key == KEY_RIGHT || key == CTRL('f')) {
    if (p->cursor < p->length)
      p->cursor = grapheme_next(p->name, p->cursor);
  } else if (key == KEY_HOME || key == CTRL('a'))
    p->cursor = 0;
  else if (key == KEY_END || key == CTRL('e'))
    p->cursor = p->length;
  else if ((key == KEY_BACKSPACE || key == CTRL('h')) && p->cursor > 0) {
    int start = grapheme_prev(p->name, p->cursor);
    memmove(p->name + start, p->name + p->cursor, p->length - p->cursor + 1);
    p->length -= p->cursor - start;
    p->cursor = start;
  } else if (key == KEY_DELETE || key == CTRL('d')) {
    if (p->cursor < p->length) {
      int end = grapheme_next(p->name, p->cursor);
      memmove(p->name + p->cursor, p->name + end, p->length - end + 1);
      p->length -= end - p->cursor;
    }
  } else if (key >= 32 && key < KEY_SPECIAL && key != KEY_BACKSPACE) {
    char seq[4];
    int n = utf8_encode(key, seq);
    if (n && p->length + n < (int)sizeof p->name) {
      memmove(p->name + p->cursor + n, p->name + p->cursor, p->length - p->cursor + 1);
      memcpy(p->name + p->cursor, seq, n);
      p->cursor += n;
      p->length += n;
    }
  }
  return 1;
}

// Keep the insertion point visible; all writes remain inside the terminal.
static int badge_width(const editor *e) {
  if (e->cols >= 16)
    return e->new_file.rename_path ? 8 : 10;
  return e->cols >= 8 ? 3 : 0;
}

static int offset(const editor *e, int *column) {
  const new_file_prompt *p = &e->new_file;
  int start = 0, width = e->cols - badge_width(e) - 1;
  if (width < 1)
    width = 1;
  int col = utf8_cols(p->name, p->cursor);
  while (col >= width && start < p->cursor) {
    col -= grapheme_width(p->name, start);
    start = grapheme_next(p->name, start);
  }
  *column = col;
  return start;
}

void new_file_draw(const editor *e, abuf *ab) {
  if (!e->new_file.active)
    return;
  screen_position(ab, 0, e->rows - 1);
  theme_append(ab, THEME_STATUS);
  const char *badge = e->new_file.rename_path ? " RENAME " : " NEW FILE ";
  int used = screen_text(ab, badge_width(e) >= 8 ? badge :
                        badge_width(e) == 3 ? e->new_file.rename_path ? "R: " : "N: " : "", e->cols), col;
  int start = offset(e, &col);
  used += screen_text(ab, e->new_file.name + start, e->cols - used);
  screen_repeat(ab, ' ', e->cols - used);
  theme_append(ab, THEME_NORMAL);
  if (e->rows > 2) {
    screen_position(ab, 0, e->rows - 2);
    const char *text = e->new_file.error[0] ? e->new_file.error : e->new_file.directory;
    if (e->new_file.error[0])
      theme_append(ab, THEME_WARNING);
    used = screen_text(ab, text, e->cols);
    screen_repeat(ab, ' ', e->cols - used);
    theme_append(ab, THEME_NORMAL);
  }
}

int new_file_cursor(const editor *e, int *x, int *y) {
  if (!e->new_file.active || e->cols < 1 || e->rows < 1)
    return 0;
  int col;
  offset(e, &col);
  int wanted = badge_width(e) + col;
  *x = wanted < e->cols ? wanted : e->cols - 1;
  *y = e->rows - 1;
  return 1;
}
