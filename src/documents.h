#ifndef DOCUMENTS_H
#define DOCUMENTS_H

struct editor;
typedef void (*document_completion)(struct editor *e, int result);

typedef struct document_request {
  char *path, *text, *title;
  int pane;
  unsigned long tab_id, revision;
  document_completion completion;
} document_request;

void documents_open(struct editor *e, const char *path, document_completion completion);
void documents_preview(struct editor *e, int pane, const char *title, const char *text,
                        document_completion completion);
void documents_preview_at(struct editor *e, unsigned long tab_id, int pane,
                          const char *title, const char *text, document_completion completion);
struct document;
struct document *documents_find(const struct editor *e, const char *path);
void documents_shutdown(struct editor *e);
int documents_editable(const struct editor *e);
int documents_unsaved_in(const struct editor *e, const char *root);
void documents_reload(struct editor *e, const char *root);
// Match the saved pathname, rather than an inode/symlink alias.
struct document *documents_at_path(const struct editor *e, const char *path);
int documents_rename_file(struct editor *e, const char *source, const char *destination);
int documents_delete_file(struct editor *e, const char *path);

#endif
