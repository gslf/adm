#ifndef FILE_TREE_H
#define FILE_TREE_H

typedef struct tree_entry {
  char *path;
  int depth, directory, expanded;
} tree_entry;

typedef struct file_tree {
  tree_entry *entries;
  int count, selected, offset;
  char error[128];
} file_tree;

int file_tree_init(file_tree *tree, const char *root);
void file_tree_free(file_tree *tree);
int file_tree_expand(file_tree *tree);
void file_tree_collapse(file_tree *tree);
void file_tree_move(file_tree *tree, int step);
void file_tree_parent(file_tree *tree);
void file_tree_scroll(file_tree *tree, int height);
const tree_entry *file_tree_selected(const file_tree *tree);

#endif
