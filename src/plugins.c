#include "plugins.h"
#include "adm_plugin.h"
#include "config.h"
#include "cursor.h"
#include "dispatch.h"
#include "screen.h"
#include "undo.h"
#include "utf8.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
typedef HMODULE library_handle;
typedef FARPROC library_symbol;
static library_handle library_open(const char *path) {
  // Absolute path; dependencies come from its directory and standard locations.
  return LoadLibraryExA(path, NULL,
                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                            LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}
static library_symbol library_find(library_handle h, const char *name) {
  return GetProcAddress(h, name);
}
static void library_close(library_handle h) { FreeLibrary(h); }
#else
#include <dlfcn.h>
typedef void *library_handle;
typedef void *library_symbol;
static library_handle library_open(const char *path) {
  return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
static library_symbol library_find(library_handle h, const char *name) {
  return dlsym(h, name);
}
static void library_close(library_handle h) { dlclose(h); }
#endif

typedef struct loaded_command {
  library_handle library;
  adm_plugin_command function;
} loaded_command;
struct plugin_state {
  adm_config *config;
  loaded_command loaded[ADM_COMMAND_LIMIT];
  int active, selected, offset;
};
static const char *api_filename(void *context) {
  editor *e = context;
  return e->view->doc ? e->view->doc->filename : NULL;
}
static const char *api_workspace(void *context) {
  return ((editor *)context)->files.workspace_root;
}
static int api_cursor(void *context, int *row, int *col) {
  editor *e = context;
  if (!e->view->doc || !row || !col)
    return -1;
  *row = e->view->cy;
  *col = e->view->cx;
  return 0;
}
static int api_line_count(void *context) {
  editor *e = context;
  return e->view->doc ? e->view->doc->buf.nlines : 0;
}
static size_t api_read_line(void *context, int row, char *out,
                            size_t capacity) {
  editor *e = context;
  if (!e->view->doc || row < 0 || row >= e->view->doc->buf.nlines ||
      (!out && capacity))
    return SIZE_MAX;
  const char *line = buffer_line(&e->view->doc->buf, row);
  size_t length = strlen(line);
  if (capacity) {
    size_t copy = length < capacity - 1 ? length : capacity - 1;
    memcpy(out, line, copy);
    out[copy] = 0;
  }
  return length;
}
static int position(editor *e, int row, int col) {
  if (!e->view->doc || row < 0 || col < 0)
    return 0;
  buffer *b = &e->view->doc->buf;
  if (!b->nlines)
    return !row && !col;
  if (row >= b->nlines)
    return 0;
  const char *line = buffer_line(b, row);
  if ((size_t)col > strlen(line))
    return 0;
  // Require grapheme boundaries so edits cannot leave the cursor inside UTF-8.
  int at = 0;
  while (at < col)
    at = grapheme_next(line, at);
  return at == col;
}
static int api_set_cursor(void *context, int row, int col) {
  editor *e = context;
  if (!position(e, row, col))
    return -1;
  e->view->cy = row;
  e->view->cx = col;
  selection_clear(e);
  cursor_mark_column(e);
  e->sidebar.focused = 0;
  return 0;
}
static int api_replace(void *context, int row, int col, int er, int ec,
                       const char *text, size_t bytes) {
  editor *e = context;
  if (!documents_editable(e) || !position(e, row, col) ||
      !position(e, er, ec) || er < row || (er == row && ec < col) || !text ||
      bytes > INT_MAX || memchr(text, 0, bytes))
    return -1;
  if (row == er && col == ec && !bytes)
    return 0;
  buffer_edit edit;
  if (buffer_replace_span(&e->view->doc->buf, row, col, er, ec, text, bytes,
                          &edit))
    return -1;
  e->view->cy = edit.new_row;
  e->view->cx = edit.new_col;
  selection_clear(e);
  cursor_mark_column(e);
  e->view->doc->dirty = 1;
  e->sidebar.focused = 0;
  dispatch_change(e);
  return 0;
}
static void api_notice(void *context, const char *text) {
  editor *e = context;
  snprintf(e->notice, sizeof e->notice, "%s", text ? text : "");
}
static const adm_plugin_api api = {ADM_PLUGIN_ABI_VERSION,
                                   sizeof(adm_plugin_api),
                                   api_filename,
                                   api_workspace,
                                   api_cursor,
                                   api_line_count,
                                   api_read_line,
                                   api_replace,
                                   api_set_cursor,
                                   api_notice};
int plugins_count(const editor *e) {
  return e->plugins && e->plugins->config ? e->plugins->config->count : 0;
}
int plugins_modal(const editor *e) { return e->plugins && e->plugins->active; }
static void invoke(editor *e, int index) {
  struct plugin_state *p = e->plugins;
  user_command *cmd = &p->config->commands[index];
  loaded_command *loaded = &p->loaded[index];
  p->active = 0;
  if (!loaded->library) {
    library_handle h = library_open(cmd->library);
    if (!h) {
      snprintf(e->notice, sizeof e->notice, "Plugin '%s': cannot load library",
               cmd->label);
      return;
    }
    library_symbol symbol = library_find(h, "adm_plugin_version");
    unsigned (*version)(void) = NULL;
    _Static_assert(sizeof version == sizeof symbol,
                   "native function pointer size");
    memcpy(&version, &symbol, sizeof version);
    if (!version || version() != ADM_PLUGIN_ABI_VERSION) {
      library_close(h);
      snprintf(e->notice, sizeof e->notice, "Plugin '%s': incompatible ABI",
               cmd->label);
      return;
    }
    symbol = library_find(h, cmd->function);
    _Static_assert(sizeof loaded->function == sizeof symbol,
                   "native function pointer size");
    memcpy(&loaded->function, &symbol, sizeof loaded->function);
    if (!loaded->function) {
      library_close(h);
      snprintf(e->notice, sizeof e->notice, "Plugin '%s': function not found",
               cmd->label);
      return;
    }
    loaded->library = h;
  }
  undo_group_begin(e);
  int result = loaded->function(&api, e);
  undo_group_end(e);
  if (result && !e->notice[0])
    snprintf(e->notice, sizeof e->notice, "Plugin '%s': command failed (%d)",
             cmd->label, result);
}
static int capacity(const editor *e) { return e->rows > 4 ? e->rows - 4 : 1; }
static void clamp(editor *e) {
  struct plugin_state *p = e->plugins;
  int count = plugins_count(e), height = capacity(e);
  if (p->selected >= count)
    p->selected = count > 0 ? count - 1 : 0;
  if (p->selected < 0)
    p->selected = 0;
  if (p->offset > p->selected)
    p->offset = p->selected;
  if (p->selected >= p->offset + height)
    p->offset = p->selected - height + 1;
  int last = count > height ? count - height : 0;
  if (p->offset > last)
    p->offset = last;
}
int plugins_key(editor *e, int key) {
  struct plugin_state *p = e->plugins;
  if (!p) {
    if (key != META('c'))
      return 0;
    snprintf(e->notice, sizeof e->notice,
             "User Center unavailable: out of memory");
    return 1;
  }
  if (key == META('c')) {
    p->active = 1;
    e->prefix_active = 0;
    clamp(e);
    return 1;
  }
  if (!p->active)
    return 0;
  if (key == '\x1b' || key == CTRL('g'))
    p->active = 0;
  else if (key == KEY_UP)
    p->selected--;
  else if (key == KEY_DOWN)
    p->selected++;
  else if (key == KEY_PGUP)
    p->selected -= capacity(e);
  else if (key == KEY_PGDOWN)
    p->selected += capacity(e);
  else if (key == KEY_HOME)
    p->selected = 0;
  else if (key == KEY_END)
    p->selected = plugins_count(e) - 1;
  else if (key == '\r' || key == '\n') {
    if (plugins_count(e))
      invoke(e, p->selected);
  } else {
    for (int i = 0; i < plugins_count(e); i++)
      if (p->config->commands[i].key == key) {
        invoke(e, i);
        break;
      }
  }
  clamp(e);
  return 1;
}
static void row(editor *e, abuf *ab, int y, const char *text,
                theme_role colour) {
  screen_position(ab, 0, y);
  theme_append(ab, colour);
  int used = screen_text(ab, text, e->cols);
  screen_repeat(ab, ' ', e->cols - used);
  theme_append(ab, THEME_NORMAL);
}
static void draw(editor *e, abuf *ab) {
  if (!plugins_modal(e) || e->rows < 3 || e->cols < 1)
    return;
  clamp(e);
  row(e, ab, 1, " USER CENTER  M-c", THEME_ACTIVE);
  int height = capacity(e), count = plugins_count(e);
  for (int i = 0; i < height && i + 2 < e->rows - 1; i++) {
    char text[160] = {0};
    int index = i + e->plugins->offset;
    if (index < count) {
      user_command *cmd = &e->plugins->config->commands[index];
      snprintf(text, sizeof text, " %c  %s", cmd->key, cmd->label);
    } else if (!count && !i)
      snprintf(text, sizeof text, " Add commands to ~/.adm.conf (see HELP.md)");
    row(e, ab, i + 2, text,
        count && index == e->plugins->selected ? THEME_ACTIVE : THEME_POPUP);
  }
  row(e, ab, e->rows - 1,
      e->cols >= 45 ? " USER  Up/Down: choose  Enter/key: run  Esc: close"
                    : " USER Up/Dn Enter/key Esc",
      THEME_STATUS);
}
static void init(editor *e) {
  theme_use(NULL);
  e->plugins = calloc(1, sizeof *e->plugins);
  if (!e->plugins) {
    snprintf(e->notice, sizeof e->notice,
             "Configuration unavailable: out of memory");
    return;
  }
  e->plugins->config = config_load();
  adm_config *c = e->plugins->config;
  if (!c) {
    snprintf(e->notice, sizeof e->notice,
             "Configuration unavailable: out of memory");
    return;
  }
  theme_use(&c->palette);
  if (c->error[0])
    snprintf(e->notice, sizeof e->notice, "%s", c->error);
}
static void shutdown(editor *e) {
  if (!e->plugins)
    return;
  for (int i = 0; i < ADM_COMMAND_LIMIT; i++)
    if (e->plugins->loaded[i].library)
      library_close(e->plugins->loaded[i].library);
  config_free(e->plugins->config);
  free(e->plugins);
  e->plugins = NULL;
  theme_use(NULL);
}
module *plugins_module(void) {
  static module m = {
      .name = "plugins", .init = init, .on_draw = draw, .shutdown = shutdown};
  return &m;
}
