#ifndef DOCUMENTS_H
#define DOCUMENTS_H

struct editor;
typedef void (*document_completion)(struct editor *e, int result);

typedef struct document_request {
  char *path, *text, *title;
  int pane;
  document_completion completion;
} document_request;

void documents_open(struct editor *e, const char *path, document_completion completion);
void documents_preview(struct editor *e, int pane, const char *title, const char *text,
                        document_completion completion);
void documents_shutdown(struct editor *e);
int documents_editable(const struct editor *e);
int documents_unsaved_in(const struct editor *e, const char *root);
void documents_reload(struct editor *e, const char *root);

#endif
