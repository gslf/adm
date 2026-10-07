#include "tabs.h"
#include "dispatch.h"
#include "screen.h"
#include "path.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

workspace *tabs_workspace(const editor *e, const editor_tab *tab) {
  return tab == e->active_tab ? (workspace *)&e->windows : (workspace *)&tab->windows;
}

editor_tab *tabs_find(const editor *e, unsigned long id) {
  for (editor_tab *tab = e->tabs; tab; tab = tab->next)
    if (tab->id == id)
      return tab;
  return NULL;
}

void tabs_init(editor *e) {
  e->first_tab = (editor_tab){.id = ++e->next_tab_id};
  e->tabs = e->active_tab = &e->first_tab;
  e->tab_count = 1;
}

static void activate(editor *e, editor_tab *tab) {
  if (tab == e->active_tab)
    return;
  e->active_tab->windows = e->windows;
  e->windows = tab->windows;
  memset(&tab->windows, 0, sizeof tab->windows);
  e->active_tab = tab;
  e->view = &e->windows.panes[e->windows.active];
  e->sidebar.focused = 0;
  if (!e->view->doc)
    sidebar_show(e, SIDEBAR_FILES);
  else
    layout_arrange(e);
}

// A new tab owns a real empty view, with no document or buffer behind it.
int tabs_new(editor *e) {
  if (e->confirmation || e->new_file.active)
    return 0;
  editor_tab *tab = calloc(1, sizeof *tab);
  if (!tab) {
    snprintf(e->notice, sizeof e->notice, "Cannot create tab: out of memory");
    return 0;
  }
  tab->id = ++e->next_tab_id;
  tab->windows.count = 1;
  tab->windows.nodes[0] = (layout_node){
    .used = 1, .parent = -1, .first = -1, .second = -1, .pane = 0
  };
  tab->windows.panes[0] = (view){.used = 1, .revision = ++e->next_view_revision};
  tab->previous = e->active_tab;
  tab->next = e->active_tab->next;
  if (tab->next)
    tab->next->previous = tab;
  e->active_tab->next = tab;
  e->tab_count++;
  activate(e, tab);
  return 1;
}

void tabs_focus(editor *e, int step) {
  if (e->confirmation || e->new_file.active || e->tab_count < 2)
    return;
  editor_tab *tab = e->active_tab;
  int moves = step % e->tab_count;
  while (moves > 0) {
    tab = tab->next ? tab->next : e->tabs;
    moves--;
  }
  while (moves < 0) {
    if (tab->previous)
      tab = tab->previous;
    else
      while (tab->next)
        tab = tab->next;
    moves++;
  }
  activate(e, tab);
}

static void close_now(editor *e) {
  editor_tab *tab = e->active_tab;
  // Closing the final tab is quitting, including any running Git operation.
  if (e->tab_count == 1) {
    e->running = 0;
    return;
  }
  editor_tab *next = tab->next ? tab->next : tab->previous;
  activate(e, next);
  if (tab->previous)
    tab->previous->next = tab->next;
  else
    e->tabs = tab->next;
  if (tab->next)
    tab->next->previous = tab->previous;
  for (int i = 0; i < MAX_PANES; i++)
    view_dispose(&tab->windows.panes[i]);
  if (tab != &e->first_tab)
    free(tab);
  else
    memset(tab, 0, sizeof *tab);
  e->tab_count--;
}

void tabs_close(editor *e) {
  if (e->confirmation || e->new_file.active)
    return;
  for (int i = 0; i < MAX_PANES; i++) {
    const document *doc = e->windows.panes[i].doc;
    if (!doc || !doc->dirty)
      continue;
    int closing = 0;
    for (int j = 0; j < MAX_PANES; j++)
      closing += e->windows.panes[j].doc == doc;
    if (doc->views == closing) {
      dispatch_confirm(e, close_now, e->tab_count == 1 ?
                       "Quit without saving?" : "Close tab without saving its buffers?");
      return;
    }
  }
  if (e->tab_count == 1 && e->git.action != GIT_IDLE)
    dispatch_confirm(e, close_now, "Quit and stop Git operation?");
  else
    close_now(e);
}

void tabs_shutdown(editor *e) {
  editor_tab *tab = e->tabs;
  while (tab) {
    editor_tab *next = tab->next;
    workspace *w = tabs_workspace(e, tab);
    for (int i = 0; i < MAX_PANES; i++)
      view_dispose(&w->panes[i]);
    if (tab != &e->first_tab)
      free(tab);
    tab = next;
  }
  e->tabs = e->active_tab = NULL;
  e->tab_count = 0;
}

static void new_tab(editor *e) { tabs_new(e); }

void tabs_bindings(void) {
  dispatch_bind(CTRL('t'), new_tab);
  dispatch_bind_prefix('t', new_tab, "t", "New empty tab");
  dispatch_bind_prefix('k', tabs_close, "k", "Close current tab");
}

void tabs_draw(const editor *e, abuf *ab, int width) {
  int active = 0;
  for (editor_tab *tab = e->tabs; tab != e->active_tab; tab = tab->next)
    active++;
  char label[256];
  snprintf(label, sizeof label, " [%d/%d] ", active + 1, e->tab_count);
  int used = screen_text(ab, label, width);
  int available = width - used;
  // Fixed-width slots keep the active tab visible even on narrow terminals.
  int slot = available < 24 ? available : 24;
  if (slot <= 0)
    return;
  int capacity = available / slot;
  int start = active >= capacity ? active - capacity + 1 : 0;
  int index = 0;
  for (editor_tab *tab = e->tabs; tab; tab = tab->next, index++) {
    if (index < start || index >= start + capacity)
      continue;
    workspace *w = tabs_workspace(e, tab);
    document *doc = w->panes[w->active].doc;
    int dirty = 0;
    for (int i = 0; i < MAX_PANES; i++)
      dirty |= w->panes[i].doc && w->panes[i].doc->dirty;
    snprintf(label, sizeof label, " %d%s %s", index + 1, dirty ? "*" : "",
             !doc ? "[Empty]" : doc->label ? doc->label : doc->filename ? path_name(doc->filename) : "[No Name]");
    const char *colour = tab == e->active_tab ? "\x1b[97;44m" : "\x1b[30;47m";
    ab_append(ab, colour, (int)strlen(colour));
    int n = screen_text(ab, label, slot);
    screen_repeat(ab, ' ', slot - n);
    used += slot;
  }
  ab_append(ab, "\x1b[30;103m", 9);
  screen_repeat(ab, ' ', width - used);
}
