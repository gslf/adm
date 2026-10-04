#include "git_repository.h"

#include <stdlib.h>
#include <string.h>

static char *copy(const char *data, size_t length) {
  char *text = malloc(length + 1);
  if (text) {
    memcpy(text, data, length);
    text[length] = '\0';
  }
  return text;
}

static void free_files(git_repository *repo) {
  for (int i = 0; i < repo->count; i++) {
    free(repo->files[i].path);
    free(repo->files[i].original);
  }
  free(repo->files);
  free(repo->head);
  repo->files = NULL;
  repo->head = NULL;
  repo->count = repo->staged = repo->unstaged = repo->conflicts = repo->unborn = 0;
}

static void free_branches(git_repository *repo) {
  for (int i = 0; i < repo->branch_count; i++)
    free(repo->branches[i].name);
  free(repo->branches);
  repo->branches = NULL;
  repo->branch_count = 0;
}

void git_repository_free(git_repository *repo) {
  free_files(repo);
  free_branches(repo);
  free(repo->root);
  *repo = (git_repository){0};
}

static int conflict(char x, char y) {
  return x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D');
}

// NUL-delimited porcelain keeps whitespace, newlines and rename paths unambiguous.
int git_repository_status(git_repository *repo, const char *data, size_t length) {
  git_repository next = {0};
  size_t offset = 0;
  int capacity = 0;
  while (offset < length) {
    const char *record = data + offset;
    const char *end = memchr(record, '\0', length - offset);
    if (!end)
      goto fail;
    size_t size = (size_t)(end - record);
    offset += size + 1;
    if (size >= 3 && !memcmp(record, "## ", 3)) {
      if (next.head)
        goto fail;
      next.head = copy(record + 3, size - 3);
      if (!next.head)
        goto fail;
      next.unborn = !strncmp(next.head, "No commits yet on ", 18) ||
                    !strncmp(next.head, "Initial commit on ", 18);
      continue;
    }
    if (size < 4 || record[2] != ' ')
      goto fail;
    if (next.count == capacity) {
      int more_capacity = capacity ? capacity * 2 : 32;
      git_file *more = realloc(next.files, (size_t)more_capacity * sizeof *more);
      if (!more)
        goto fail;
      next.files = more;
      capacity = more_capacity;
    }
    git_file *file = &next.files[next.count++];
    *file = (git_file){.index = record[0], .worktree = record[1]};
    file->path = copy(record + 3, size - 3);
    if (!file->path)
      goto fail;
    if (file->index == 'R' || file->index == 'C' ||
        file->worktree == 'R' || file->worktree == 'C') {
      const char *original = data + offset;
      const char *original_end = memchr(original, '\0', length - offset);
      if (!original_end || original_end == original)
        goto fail;
      file->original = copy(original, (size_t)(original_end - original));
      if (!file->original)
        goto fail;
      offset += (size_t)(original_end - original) + 1;
    }
    file->conflict = conflict(file->index, file->worktree);
    file->untracked = file->index == '?' && file->worktree == '?';
    if (file->conflict)
      next.conflicts++;
    else {
      next.staged += file->index != ' ' && file->index != '?' && file->index != '!';
      next.unstaged += file->untracked || (file->worktree != ' ' && file->worktree != '!');
    }
  }
  if (!next.head)
    goto fail;
  free_files(repo);
  repo->head = next.head;
  repo->files = next.files;
  repo->count = next.count;
  repo->staged = next.staged;
  repo->unstaged = next.unstaged;
  repo->conflicts = next.conflicts;
  repo->unborn = next.unborn;
  return 0;
fail:
  free_files(&next);
  return -1;
}

int git_repository_branches(git_repository *repo, const char *data, size_t length) {
  git_repository next = {0};
  size_t offset = 0;
  while (offset < length) {
    const char *line = data + offset;
    const char *end = memchr(line, '\n', length - offset);
    size_t size = end ? (size_t)(end - line) : length - offset;
    offset += size + (end != NULL);
    if (size && line[size - 1] == '\r')
      size--;
    int remote = size > 13 && !memcmp(line, "refs/remotes/", 13);
    size_t prefix = remote ? 13 : 11;
    if ((!remote && (size <= 11 || memcmp(line, "refs/heads/", 11))) ||
        (remote && size >= 5 && !memcmp(line + size - 5, "/HEAD", 5)))
      continue;
    git_branch *more = realloc(next.branches, ((size_t)next.branch_count + 1) * sizeof *more);
    if (!more)
      goto fail;
    next.branches = more;
    char *name = copy(line + prefix, size - prefix);
    if (!name)
      goto fail;
    more[next.branch_count++] = (git_branch){name, remote};
  }
  free_branches(repo);
  repo->branches = next.branches;
  repo->branch_count = next.branch_count;
  return 0;
fail:
  free_branches(&next);
  return -1;
}

int git_repository_items(const git_repository *repo) {
  return repo->conflicts + repo->staged + repo->unstaged;
}

git_item git_repository_item(const git_repository *repo, int index) {
  for (int section = GIT_CONFLICT; section <= GIT_UNSTAGED; section++)
    for (int i = 0; i < repo->count; i++) {
      const git_file *file = &repo->files[i];
      int included = section == GIT_CONFLICT ? file->conflict :
                     file->conflict ? 0 : section == GIT_STAGED ?
                     file->index != ' ' && file->index != '?' && file->index != '!' :
                     file->untracked || (file->worktree != ' ' && file->worktree != '!');
      if (included && index-- == 0)
        return (git_item){file, (git_section)section};
    }
  return (git_item){0};
}
