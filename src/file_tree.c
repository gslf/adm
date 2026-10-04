#include "file_tree.h"
#include "path.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

static void free_entries(tree_entry *entries, int count) {
  for (int i = 0; i < count; i++)
    free(entries[i].path);
  free(entries);
}

static int add_entry(tree_entry **entries, int *count, int *capacity, const char *root,
                     const char *name, int depth, int directory) {
  if (!strcmp(name, ".") || !strcmp(name, ".."))
    return 0;
  if (*count == INT_MAX) {
    errno = ENOMEM;
    return -1;
  }
  char *path = path_join(root, name);
  if (!path)
    return -1;
#ifndef _WIN32
  struct stat info;
  if (stat(path, &info) == 0)
    directory = S_ISDIR(info.st_mode);
#endif
  if (*count == *capacity) {
    int next = *capacity > INT_MAX / 2 ? INT_MAX : *capacity ? *capacity * 2 : 32;
    tree_entry *more = realloc(*entries, (size_t)next * sizeof **entries);
    if (!more) {
      free(path);
      return -1;
    }
    *entries = more;
    *capacity = next;
  }
  (*entries)[(*count)++] = (tree_entry){path, depth, directory, 0};
  return 0;
}

static int compare_entries(const void *left, const void *right) {
  const tree_entry *a = left, *b = right;
  if (a->directory != b->directory)
    return b->directory - a->directory;
  return strcmp(path_name(a->path), path_name(b->path));
}

static int scan(const tree_entry *parent, tree_entry **entries, int *count) {
  int result = 0, capacity = 0;
#ifdef _WIN32
  char *pattern = path_join(parent->path, "*");
  if (!pattern)
    return -1;
  WIN32_FIND_DATAA data;
  HANDLE handle = FindFirstFileA(pattern, &data);
  free(pattern);
  if (handle == INVALID_HANDLE_VALUE) {
    if (GetLastError() == ERROR_FILE_NOT_FOUND)
      return 0;
    errno = EACCES;
    return -1;
  }
  do {
    if (add_entry(entries, count, &capacity, parent->path, data.cFileName, parent->depth + 1,
                  !!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) < 0) {
      result = -1;
      break;
    }
  } while (FindNextFileA(handle, &data));
  if (result == 0 && GetLastError() != ERROR_NO_MORE_FILES) {
    errno = EIO;
    result = -1;
  }
  FindClose(handle);
#else
  DIR *directory = opendir(parent->path);
  if (!directory)
    return -1;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(directory);
    if (!entry) {
      result = errno ? -1 : 0;
      break;
    }
    if (add_entry(entries, count, &capacity, parent->path, entry->d_name,
                  parent->depth + 1, 0) < 0) {
      result = -1;
      break;
    }
  }
  int error = errno;
  closedir(directory);
  errno = error;
#endif
  if (result == 0 && *count > 1)
    qsort(*entries, (size_t)*count, sizeof **entries, compare_entries);
  return result;
}

const tree_entry *file_tree_selected(const file_tree *tree) {
  return tree->count ? &tree->entries[tree->selected] : NULL;
}

int file_tree_init(file_tree *tree, const char *root) {
  char *path = path_absolute(root);
  tree_entry *entry = path ? malloc(sizeof *entry) : NULL;
  if (!entry) {
    free(path);
    snprintf(tree->error, sizeof tree->error, "Cannot open tree root");
    return -1;
  }
  *entry = (tree_entry){path, 0, 1, 0};
  tree->entries = entry;
  tree->count = 1;
  return file_tree_expand(tree);
}

void file_tree_free(file_tree *tree) {
  free_entries(tree->entries, tree->count);
  *tree = (file_tree){0};
}

// Only expanded directories occupy rows; scanning never walks the whole project.
int file_tree_expand(file_tree *tree) {
  const tree_entry *parent = file_tree_selected(tree);
  if (!parent || !parent->directory || parent->expanded)
    return 0;
  tree_entry *children = NULL;
  int count = 0;
  if (scan(parent, &children, &count) < 0 || count > INT_MAX - tree->count) {
    snprintf(tree->error, sizeof tree->error, "Cannot read directory: %s",
             strerror(errno));
    free_entries(children, count);
    return -1;
  }
  tree_entry *more = realloc(tree->entries,
                             ((size_t)tree->count + count) * sizeof *more);
  if (!more) {
    free_entries(children, count);
    snprintf(tree->error, sizeof tree->error, "Out of memory");
    return -1;
  }
  tree->entries = more;
  int after = tree->selected + 1;
  memmove(more + after + count, more + after,
          (size_t)(tree->count - after) * sizeof *more);
  if (count)
    memcpy(more + after, children, (size_t)count * sizeof *more);
  free(children);
  tree->count += count;
  more[tree->selected].expanded = 1;
  tree->error[0] = '\0';
  return 0;
}

void file_tree_collapse(file_tree *tree) {
  tree_entry *parent = tree->count ? &tree->entries[tree->selected] : NULL;
  if (!parent || !parent->expanded)
    return;
  int after = tree->selected + 1, end = after;
  while (end < tree->count && tree->entries[end].depth > parent->depth)
    free(tree->entries[end++].path);
  memmove(tree->entries + after, tree->entries + end,
          (size_t)(tree->count - end) * sizeof *tree->entries);
  tree->count -= end - after;
  parent->expanded = 0;
  tree->error[0] = '\0';
}

void file_tree_move(file_tree *tree, int step) {
  if (!tree->count)
    return;
  if (step < -tree->selected)
    tree->selected = 0;
  else if (step >= tree->count - tree->selected)
    tree->selected = tree->count - 1;
  else
    tree->selected += step;
  tree->error[0] = '\0';
}

void file_tree_parent(file_tree *tree) {
  const tree_entry *entry = file_tree_selected(tree);
  if (!entry)
    return;
  int depth = entry->depth;
  while (tree->selected > 0) {
    tree->selected--;
    if (tree->entries[tree->selected].depth < depth)
      break;
  }
}

void file_tree_scroll(file_tree *tree, int height) {
  if (height < 1) {
    tree->offset = 0;
    return;
  }
  if (tree->selected < tree->offset)
    tree->offset = tree->selected;
  if (tree->selected >= tree->offset + height)
    tree->offset = tree->selected - height + 1;
  int last = tree->count > height ? tree->count - height : 0;
  if (tree->offset > last)
    tree->offset = last;
}
