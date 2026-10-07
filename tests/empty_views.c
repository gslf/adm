#include "dispatch.h"
#include "clipboard.h"
#include "search.h"
#include "screen.h"
#include "path.h"
#include "utf8.h"
#include "fileio.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void prefix(editor *e, int key) {
  dispatch_key(e, CTRL('x'));
  dispatch_key(e, key);
  assert(!e->prefix_active);
}

static void type(editor *e, const char *text) {
  while (*text) {
    int cp;
    text += utf8_decode(text, &cp);
    dispatch_key(e, cp);
  }
}

static void validate(editor *e) {
  int count = 0;
  for (editor_tab *tab = e->tabs; tab; tab = tab->next) {
    workspace *w = tabs_workspace(e, tab);
    int used = 0;
    for (int i = 0; i < MAX_PANES; i++) {
      view *v = &w->panes[i];
      used += v->used;
      assert(!v->used || v->revision);
      assert(!v->doc || v->used);
      if (v->doc) {
        int references = 0;
        for (editor_tab *other = e->tabs; other; other = other->next)
          for (int j = 0; j < MAX_PANES; j++)
            references += tabs_workspace(e, other)->panes[j].doc == v->doc;
        assert(v->doc->views == references);
      }
    }
    assert(used == w->count && used >= 1 && used <= 4);
    assert(w->panes[w->active].used);
    count++;
  }
  assert(count == e->tab_count && e->view->used);
}

static void select_folder(editor *e) {
  sidebar_show(e, SIDEBAR_FILES);
  for (int i = 0; i < e->files.tree.count; i++)
    if (!strcmp(path_name(e->files.tree.entries[i].path), "folder")) {
      e->files.tree.selected = i;
      return;
    }
  assert(!"Missing fixture folder");
}

int main(int argc, char **argv) {
  assert(argc == 2);
  (void)argv;
  editor e = {.rows = 40, .cols = 120, .running = 1};
  dispatch_register(clipboard_module());
  dispatch_register(search_module());
  dispatch_init(&e);
  assert(!e.view->doc && !e.document.views && !e.document.buf.head);
  assert(e.sidebar.kind == SIDEBAR_FILES && e.sidebar.focused);
  assert(e.files.tree.entries[0].directory && e.view->used);
  assert(dispatch_prefix_find(&e, 'N'));
  abuf ab = {0};
  screen_empty(&ab, (rect){0, 0, 100, 20});
  ab_append(&ab, "\0", 1);
  assert(strstr(ab.b, "\x1b[1;93m") && strstr(ab.b, "Create new file"));
  ab_free(&ab);
  for (int width = 1; width < 40; width++) {
    screen_empty(&ab, (rect){0, 0, width, 2});
    ab_free(&ab);
  }
  dispatch_key(&e, CTRL('g'));
  assert(!e.sidebar.focused);
  const int keys[] = {'X', '\n', KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
      KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDOWN, CTRL('d'), CTRL('h'),
      CTRL(' '), CTRL('s'), CTRL('w'), CTRL('y'), META('w')};
  for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
    dispatch_key(&e, keys[i]);
  prefix(&e, 'l');
  prefix(&e, CTRL('s'));
  assert(!e.view->doc && !layout_has_unsaved(&e) && !e.view->sel_mode);

  // Every opened sidebar takes focus, including switching from Git to Files.
  sidebar_toggle(&e, SIDEBAR_FILES);
  assert(e.sidebar.kind == SIDEBAR_NONE && !e.sidebar.focused);
  prefix(&e, 'f');
  assert(e.sidebar.kind == SIDEBAR_FILES && e.sidebar.focused);
  sidebar_show(&e, SIDEBAR_GIT);
  assert(e.sidebar.kind == SIDEBAR_GIT && e.sidebar.focused);
  prefix(&e, 'f');
  assert(e.sidebar.kind == SIDEBAR_FILES && e.sidebar.focused);
  prefix(&e, 'f');
  prefix(&e, 'f');
  assert(e.sidebar.kind == SIDEBAR_FILES && e.sidebar.focused);

  prefix(&e, 'u');
  assert(e.windows.count == 2 && e.windows.active == 1 && !e.view->doc);
  assert(e.sidebar.kind == SIDEBAR_FILES && e.sidebar.focused);
  prefix(&e, 'q');
  prefix(&e, 'q');
  assert(e.windows.count == 4 && !layout_split(&e, LAYOUT_VERTICAL));
  validate(&e);
  unsigned long first = e.active_tab->id;
  prefix(&e, 't');
  assert(e.windows.count == 1 && e.tab_count == 2 && !e.view->doc);
  assert(e.sidebar.focused && !e.document.views);
  tabs_focus(&e, -1);
  assert(e.active_tab->id == first && e.windows.count == 4);
  tabs_focus(&e, 1);
  prefix(&e, 'k');
  assert(e.tab_count == 1 && !e.confirmation);
  layout_only(&e);
  assert(e.windows.count == 1 && !e.view->doc);
  validate(&e);

  // Cancel and validation failures leave the view truly empty, without files.
  char *root = path_current_directory();
  char *created = path_join(root, "created è.txt");
  assert(root && created);
  prefix(&e, 'N');
  type(&e, "created è.txt");
  dispatch_key(&e, CTRL('g'));
  struct stat info;
  assert(!e.new_file.active && !e.view->doc && e.sidebar.focused && stat(created, &info) < 0);
  prefix(&e, 'N');
  dispatch_key(&e, '\r');
  assert(e.new_file.active && e.new_file.error[0]);
  type(&e, "folder/relative.txt");
  dispatch_key(&e, '\r');
  assert(e.new_file.active && strstr(e.new_file.error, "absolute") && !e.view->doc);
  dispatch_key(&e, '\x1b');
  prefix(&e, 'N');
  type(&e, "existing.txt");
  dispatch_key(&e, '\r');
  assert(e.new_file.active && strstr(e.new_file.error, "already exists") && !e.view->doc);
  char *old = file_read("existing.txt");
  assert(old && !strcmp(old, "keep\n"));
  free(old);
  dispatch_key(&e, CTRL('g'));
  prefix(&e, 'N');
  type(&e, "created é.txt");
  // Backspace, home/end, and UTF-8 editing never split a code point.
  for (int i = 0; i < 4; i++)
    dispatch_key(&e, KEY_LEFT);
  dispatch_key(&e, CTRL('h'));
  dispatch_key(&e, 0xe8);
  assert(!strcmp(e.new_file.name, "created è.txt"));
  dispatch_key(&e, KEY_END);
  dispatch_key(&e, '\r');
  assert(!e.new_file.active && e.view->doc && !e.sidebar.focused);
  assert(!e.view->doc->dirty && stat(created, &info) == 0 && info.st_size == 0);
  assert(!strcmp(e.view->doc->filename, created));
  assert(!dispatch_prefix_find(&e, 'N'));
  dispatch_key(&e, 'Q');
  document *shared = e.view->doc;
  assert(shared->dirty && shared->views == 1);
  prefix(&e, 't');
  assert(!e.view->doc && shared->views == 1 && e.sidebar.focused);
  documents_open(&e, created, NULL);
  assert(e.view->doc == shared && shared->views == 2 && !e.confirmation);
  dispatch_key(&e, '!');
  assert(!strcmp(buffer_line(&shared->buf, 0), "!Q"));
  prefix(&e, 'u');
  assert(!e.view->doc && shared->views == 2);
  documents_open(&e, created, NULL);
  assert(e.view->doc == shared && shared->views == 3);
  prefix(&e, 'k');
  assert(!e.confirmation && shared->views == 1);

  // Simple names follow the selected folder; absolute paths bypass it.
  prefix(&e, 't');
  select_folder(&e);
  prefix(&e, 'N');
  type(&e, "nested.txt");
  dispatch_key(&e, '\r');
  char *nested = path_join(root, "folder/nested.txt");
  assert(e.view->doc && !strcmp(e.view->doc->filename, nested) && stat(nested, &info) == 0);
  prefix(&e, 't');
  select_folder(&e);
  prefix(&e, 'N');
  type(&e, argv[1]);
  dispatch_key(&e, '\r');
  assert(e.view->doc && !strcmp(e.view->doc->filename, argv[1]));
  prefix(&e, 't');
  prefix(&e, 'N');
  type(&e, "/missing-adm-parent/file.txt");
  dispatch_key(&e, '\r');
  assert(!e.view->doc && e.new_file.active && e.new_file.error[0]);
  dispatch_key(&e, CTRL('g'));
  // A delayed result cannot take the new-file prompt's target.
  prefix(&e, 'N');
  documents_preview(&e, e.windows.active, "Diff", "test\n", NULL);
  assert(e.new_file.active && !e.view->doc && !e.open_request.text);
  dispatch_key(&e, CTRL('g'));
  validate(&e);
  dispatch_shutdown(&e);
  assert(!e.view && !e.new_file.directory && !e.document.views);
  free(root);
  free(created);
  free(nested);
  puts("Empty-view and new-file regressions passed.");
  return 0;
}
