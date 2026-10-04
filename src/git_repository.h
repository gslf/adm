#ifndef GIT_REPOSITORY_H
#define GIT_REPOSITORY_H

#include <stddef.h>

typedef enum { GIT_CONFLICT, GIT_STAGED, GIT_UNSTAGED } git_section;

typedef struct git_file {
  char *path, *original;
  char index, worktree;
  int conflict, untracked;
} git_file;

typedef struct git_branch {
  char *name;
  int remote;
} git_branch;

typedef struct git_repository {
  char *root, *head;
  git_file *files;
  int count, staged, unstaged, conflicts, unborn;
  git_branch *branches;
  int branch_count;
} git_repository;

typedef struct git_item {
  const git_file *file;
  git_section section;
} git_item;

int git_repository_status(git_repository *repo, const char *data, size_t length);
int git_repository_branches(git_repository *repo, const char *data, size_t length);
int git_repository_items(const git_repository *repo);
git_item git_repository_item(const git_repository *repo, int index);
void git_repository_free(git_repository *repo);

#endif
