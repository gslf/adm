#include "shared_views.h"
#include "dispatch.h"

#include <assert.h>
#include <string.h>

static void type(editor *e, const char *text) {
  while (*text)
    dispatch_key(e, *text == '\n' ? (++text, '\r') : (unsigned char)*text++);
}

static void command_key(editor *e, int key) {
  test_prefix_shared(e, key);
}

static void closed(const document *doc) {
  assert(doc->views == 0 && !doc->buf.head && !doc->buf.tail);
  assert(doc->buf.nlines == 0 && !doc->buf.on_edit && !doc->filename);
}

int main(int argc, char **argv) {
  assert(argc == 2);
  editor e = {.document.filename = argv[1], .rows = 24, .cols = 80, .running = 1};
  document other = {.filename = "other"}, third = {.filename = "third"};
  buffer_init(&e.document.buf);
  dispatch_init(&e);
  type(&e, "A");
  command_key(&e, 'u');
  assert(e.document.views == 2);
  layout_set_document(&e, &other);
  type(&e, "B");
  assert(other.views == 1 && e.document.views == 1);
  assert(!strcmp(buffer_line(&e.document.buf, 0), "A"));
  command_key(&e, 'c');
  assert(e.confirmation && e.windows.count == 2 && other.buf.head);
  dispatch_key(&e, 'n');
  assert(!e.confirmation && other.views == 1 && e.view->doc == &other);
  command_key(&e, 'c');
  dispatch_key(&e, 'y');
  closed(&other);
  assert(e.windows.count == 1 && e.view->doc == &e.document && e.document.views == 1);

  other.filename = "other";
  command_key(&e, 'q');
  layout_focus(&e, 1);
  layout_set_document(&e, &other);
  type(&e, "shared");
  command_key(&e, 'u');
  assert(other.views == 2);
  command_key(&e, 'c');
  assert(!e.confirmation && e.windows.count == 2 && other.views == 1);
  assert(!strcmp(buffer_line(&other.buf, 0), "shared"));
  command_key(&e, 'c');
  dispatch_key(&e, KEY_DELETE);
  assert(!e.confirmation && other.views == 1 && other.buf.head);
  command_key(&e, 'c');
  dispatch_key(&e, 'y');
  closed(&other);
  assert(e.document.views == 1 && e.windows.count == 1);

  other.filename = "other";
  command_key(&e, 'u');
  layout_focus(&e, 1);
  layout_set_document(&e, &other);
  type(&e, "B");
  command_key(&e, 'q');
  layout_focus(&e, 1);
  layout_set_document(&e, &third);
  type(&e, "C");
  third.dirty = 0;
  assert(e.windows.count == 3 && e.view->doc == &third);
  command_key(&e, CTRL('c'));
  assert(e.confirmation && e.running); // Dirty buffers in other panes count too.
  dispatch_key(&e, 'n');
  assert(!e.confirmation && e.running);
  layout_only(&e);
  assert(e.confirmation && e.windows.count == 3);
  dispatch_key(&e, 'n');
  assert(e.document.views == 1 && other.views == 1 && third.views == 1);
  layout_only(&e);
  dispatch_key(&e, 'y');
  closed(&e.document);
  closed(&other);
  assert(e.windows.count == 1 && third.views == 1 && e.view->doc == &third);
  assert(!strcmp(buffer_line(&third.buf, 0), "C"));

  other.filename = "replacement";
  layout_set_document(&e, &other); // Replacing a clean last view releases it immediately.
  closed(&third);
  assert(other.views == 1 && e.view->doc == &other);
  type(&e, "final");
  command_key(&e, CTRL('c'));
  dispatch_key(&e, 'y');
  assert(!e.running && !e.confirmation);
  dispatch_shutdown(&e);
  closed(&other);
  assert(e.windows.count == 0 && !e.view);
  return 0;
}
