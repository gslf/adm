#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

#include "file_tree.h"
#include "view.h"

typedef struct file_manager {
  file_tree tree;
  char *workspace_root;
  char *delete_path;
  char delete_prompt[256];
} file_manager;

struct editor;
void file_manager_init(struct editor *e);
void file_manager_shutdown(struct editor *e);
void file_manager_refresh(struct editor *e);
void file_manager_refresh_select(struct editor *e, const char *path);
void file_manager_key(struct editor *e, int key);
rect file_manager_area(const struct editor *e);
void file_manager_bindings(void);

#endif
