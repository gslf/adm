#ifndef NEW_FILE_H
#define NEW_FILE_H

typedef struct new_file_prompt {
  int active, length, cursor, pane;
  unsigned long tab_id, revision;
  char name[4096], error[128];
  char *directory;
  char *rename_path;
} new_file_prompt;

struct editor;
struct abuf;
void new_file_bindings(void);
void new_file_rename(struct editor *e, const char *path);
int new_file_key(struct editor *e, int key);
void new_file_draw(const struct editor *e, struct abuf *ab);
int new_file_cursor(const struct editor *e, int *x, int *y);
void new_file_shutdown(struct editor *e);

#endif
