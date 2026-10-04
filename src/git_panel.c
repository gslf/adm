#include "git_panel.h"
#include "dispatch.h"
#include "path.h"
#include "utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const git_environment[] = {
  "LC_ALL=C", "GIT_TERMINAL_PROMPT=0", "GIT_MERGE_AUTOEDIT=no",
  "GIT_OPTIONAL_LOCKS=0",
  "GIT_PAGER=cat", "GIT_EDITOR=false", "SSH_ASKPASS_REQUIRE=never", NULL
};

static char *copy(const char *text) {
  char *result = malloc(strlen(text) + 1);
  if (result)
    strcpy(result, text);
  return result;
}

static void message(git_panel *panel, int failed, const char *text) {
  text += strspn(text, "\r\n");
  size_t length = strcspn(text, "\r\n");
  if (length >= sizeof panel->message)
    length = sizeof panel->message - 1;
  memcpy(panel->message, text, length);
  panel->message[length] = '\0';
  panel->failed = failed;
}

static int start(editor *e, git_action action, const char *directory,
                  const char *const arguments[]) {
  git_panel *panel = &e->git;
  if (panel->action != GIT_IDLE)
    return -1;
  const char *argv[32] = {"git", "--literal-pathspecs", "-c", "color.ui=false",
                         "-c", "credential.interactive=false", "-C", directory};
  int index = 8;
  for (int i = 0; arguments[i]; i++) {
    if (index >= (int)(sizeof argv / sizeof argv[0]) - 1)
      return -1;
    argv[index++] = arguments[i];
  }
  argv[index] = NULL;
  if (process_start(&panel->process, argv, git_environment) < 0) {
    message(panel, 1, "Cannot start Git");
    return -1;
  }
  panel->action = action;
  return 0;
}

static void refresh(editor *e) {
  git_panel *panel = &e->git;
  if (panel->action != GIT_IDLE)
    return;
  if (!panel->repo.root) {
    char *cwd = path_current_directory();
    const char *args[] = {"rev-parse", "--show-toplevel", NULL};
    if (cwd) {
      start(e, GIT_DISCOVER, cwd, args);
      free(cwd);
    } else
      message(panel, 1, "Cannot find current directory");
  } else {
    const char *args[] = {"status", "--porcelain=v1", "-z", "--branch",
                          "--untracked-files=all", NULL};
    start(e, GIT_STATUS, panel->repo.root, args);
  }
}

void git_panel_refresh(editor *e) {
  refresh(e);
}

void git_panel_init(editor *e) {
  e->git = (git_panel){0};
}

void git_panel_shutdown(editor *e) {
  process_dispose(&e->git.process);
  git_repository_free(&e->git.repo);
  free(e->git.action_path);
  e->git = (git_panel){0};
}

int git_panel_worktree_busy(const editor *e) {
  git_action action = e->git.action;
  return action == GIT_PULL || action == GIT_CHECKOUT ||
         action == GIT_CREATE_BRANCH || action == GIT_MERGE;
}

int git_panel_prompt(const editor *e) {
  return e->sidebar.focused && (e->git.mode == GIT_MESSAGE || e->git.mode == GIT_NEW_BRANCH);
}

int git_panel_footer_height(const editor *e) {
  int available = sidebar_area(e).height - 3;
  int wanted = e->git.mode == GIT_FILES ? 8 :
               e->git.mode == GIT_MESSAGE || e->git.mode == GIT_NEW_BRANCH ? 4 : 3;
  return available < 0 ? 0 : available < wanted ? available : wanted;
}

int git_panel_list_height(const editor *e) {
  int height = sidebar_area(e).height - 2 - git_panel_footer_height(e);
  return height > 0 ? height : 0;
}

int git_panel_selected_row(const git_panel *panel) {
  git_item item = git_repository_item(&panel->repo, panel->selected);
  if (!item.file)
    return 0;
  int headers = item.section == GIT_CONFLICT ? 1 :
                item.section == GIT_STAGED ? 1 + (panel->repo.conflicts > 0) :
                2 + (panel->repo.conflicts > 0);
  return panel->selected + headers;
}

static void opened(editor *e, int result) {
  if (result == 0)
    message(&e->git, 1, "Cannot open file or diff");
}

static void restore_selection(git_panel *panel, const char *path, git_section section) {
  int count = git_repository_items(&panel->repo), match = -1;
  for (int i = 0; path && i < count; i++) {
    git_item item = git_repository_item(&panel->repo, i);
    if (!strcmp(path, item.file->path)) {
      if (match < 0 || item.section == section)
        match = i;
    }
  }
  if (match >= 0)
    panel->selected = match;
  if (panel->selected >= count)
    panel->selected = count > 0 ? count - 1 : 0;
}

void git_panel_tick(editor *e) {
  git_panel *panel = &e->git;
  if (panel->action == GIT_IDLE || !process_poll(&panel->process))
    return;
  git_action action = panel->action;
  panel->action = GIT_IDLE;
  child_process *process = &panel->process;
  const char *output = process->out.data ? process->out.data : "";
  const char *error = process->err.data ? process->err.data : "";
  int success = !process->failed && !process->cancelled &&
      (process->exit_code == 0 || (action == GIT_DIFF && panel->diff_untracked && process->exit_code == 1));
  if (!success) {
    message(panel, 1, process->cancelled ? "Git operation cancelled or timed out" :
            process->failed ? "Git output exceeded the capture limit" :
            *error ? error : *output ? output : "Git command failed (is Git installed?)");
    if (action == GIT_COMMIT)
      panel->mode = GIT_MESSAGE;
    else if (action == GIT_CREATE_BRANCH)
      panel->mode = GIT_NEW_BRANCH;
  }
  if (action == GIT_DISCOVER && success) {
    size_t length = process->out.length;
    if (length && output[length - 1] == '\n')
      length--;
#ifdef _WIN32
    if (length && output[length - 1] == '\r')
      length--;
#endif
    char *root = malloc(length + 1);
    if (root && length) {
      memcpy(root, output, length);
      root[length] = '\0';
      free(panel->repo.root);
      panel->repo.root = root;
      message(panel, 0, "Git uses saved files");
    } else {
      free(root);
      success = 0;
      message(panel, 1, "Cannot locate repository");
    }
  } else if (action == GIT_STATUS && success) {
    git_item selected = git_repository_item(&panel->repo, panel->selected);
    char *path = selected.file ? copy(selected.file->path) : NULL;
    if (git_repository_status(&panel->repo, output, process->out.length) < 0)
      message(panel, 1, "Cannot parse Git status");
    else
      restore_selection(panel, path, selected.section);
    free(path);
  } else if (action == GIT_BRANCHES && success) {
    if (git_repository_branches(&panel->repo, output, process->out.length) < 0)
      message(panel, 1, "Cannot read branches");
    panel->branch_selected = panel->branch_offset = 0;
  } else if (action == GIT_DIFF && success) {
    if (panel->diff_pane >= 0 && panel->diff_pane < MAX_PANES &&
        e->windows.panes[panel->diff_pane].doc &&
        e->windows.panes[panel->diff_pane].revision == panel->diff_revision) {
      char title[1024];
      snprintf(title, sizeof title, "Diff [%s]: %s", panel->diff_section == GIT_STAGED ? "staged" :
               panel->diff_section == GIT_CONFLICT ? "conflict" : "changes", panel->action_path);
      documents_preview(e, panel->diff_pane, title, *output ? output : "No differences.\n", opened);
    } else
      message(panel, 1, "Diff target pane changed; try again");
  } else if (action >= GIT_STAGE && action <= GIT_MERGE) {
    if (success) {
      message(panel, 0, *output ? output : "Git operation completed");
      if (action == GIT_COMMIT) {
        panel->input[0] = '\0';
        panel->input_length = panel->input_cursor = panel->input_offset = 0;
      }
    }
    if (action == GIT_PULL || action == GIT_CHECKOUT || action == GIT_CREATE_BRANCH || action == GIT_MERGE) {
      documents_reload(e, panel->repo.root);
      if (e->sidebar.kind == SIDEBAR_FILES)
        file_manager_refresh(e);
      else
        file_tree_free(&e->files.tree);
    }
  }
  free(panel->action_path);
  panel->action_path = NULL;
  process_dispose(process);
  if ((action == GIT_DISCOVER && success) || (action >= GIT_STAGE && action <= GIT_MERGE))
    refresh(e);
}

static void toggle(editor *e) {
  sidebar_toggle(e, SIDEBAR_GIT);
  if (e->sidebar.kind == SIDEBAR_GIT)
    e->sidebar.focused = sidebar_area(e).width > 0;
}

static int saved(editor *e) {
  if (!documents_unsaved_in(e, e->git.repo.root))
    return 1;
  message(&e->git, 1, "Save modified buffers first (C-x C-s)");
  return 0;
}

static void selected_file(editor *e, int diff) {
  git_panel *panel = &e->git;
  git_item item = git_repository_item(&panel->repo, panel->selected);
  if (!item.file)
    return;
  char *path = path_join(panel->repo.root, item.file->path);
  if (!path) {
    message(panel, 1, "Out of memory");
    return;
  }
  if (!diff) {
    documents_open(e, path, opened);
    free(path);
    return;
  }
  const char *args[16];
  int index = 0;
  args[index++] = "diff";
  panel->diff_untracked = item.file->untracked;
  if (item.file->untracked)
    args[index++] = "--no-index";
  else if (item.section == GIT_STAGED)
    args[index++] = "--cached";
  args[index++] = "--no-color";
  args[index++] = "--no-ext-diff";
  args[index++] = "--no-textconv";
  args[index++] = "--";
  if (item.file->untracked)
#ifdef _WIN32
    args[index++] = "NUL";
#else
    args[index++] = "/dev/null";
#endif
  if (!item.file->untracked && item.file->original)
    args[index++] = item.file->original;
  args[index++] = item.file->untracked ? path : item.file->path;
  args[index] = NULL;
  panel->action_path = copy(item.file->path);
  panel->diff_pane = e->windows.active;
  panel->diff_revision = e->view->revision;
  panel->diff_section = item.section;
  if (!panel->action_path || start(e, GIT_DIFF, panel->repo.root, args) < 0) {
    free(panel->action_path);
    panel->action_path = NULL;
  }
  free(path);
}

static void stage(editor *e, int unstage, int all) {
  git_panel *panel = &e->git;
  git_item item = git_repository_item(&panel->repo, panel->selected);
  if (!all && (!item.file || (unstage && item.section != GIT_STAGED)))
    return;
  if (!unstage) {
    if (all && !saved(e))
      return;
    if (!all) {
      char *path = path_join(panel->repo.root, item.file->path);
      if (!path) {
        message(panel, 1, "Out of memory");
        return;
      }
      int dirty = 0;
      for (int i = 0; i < MAX_PANES; i++) {
        document *doc = e->windows.panes[i].doc;
        dirty |= doc && doc->dirty && document_matches(doc, path);
      }
      free(path);
      if (dirty) {
        message(panel, 1, "Save this buffer before staging");
        return;
      }
    }
  }
  const char *args[12];
  int index = 0;
  if (!unstage) {
    args[index++] = "add";
    args[index++] = "-A";
  } else if (panel->repo.unborn) {
    args[index++] = "rm";
    args[index++] = "-r";
    args[index++] = "--cached";
    args[index++] = "--force";
    args[index++] = "--ignore-unmatch";
  } else {
    args[index++] = "restore";
    args[index++] = "--staged";
  }
  args[index++] = "--";
  if (!all && item.file->original)
    args[index++] = item.file->original;
  args[index++] = all ? "." : item.file->path;
  args[index] = NULL;
  start(e, unstage ? all ? GIT_UNSTAGE_ALL : GIT_UNSTAGE : all ? GIT_STAGE_ALL : GIT_STAGE,
        panel->repo.root, args);
}

static void branches(editor *e, int merge) {
  if (!saved(e))
    return;
  e->git.mode = merge ? GIT_PICK_MERGE : GIT_PICK_CHECKOUT;
  const char *args[] = {"for-each-ref", "--format=%(refname)", "refs/heads", "refs/remotes", NULL};
  start(e, GIT_BRANCHES, e->git.repo.root, args);
}

static void begin_input(git_panel *panel, git_mode mode) {
  panel->mode = mode;
  panel->input[0] = '\0';
  panel->input_length = panel->input_cursor = panel->input_offset = 0;
}

static void activate_branch(editor *e) {
  git_panel *panel = &e->git;
  int create = panel->mode == GIT_PICK_CHECKOUT;
  if (create && panel->branch_selected == 0) {
    begin_input(panel, GIT_NEW_BRANCH);
    return;
  }
  int index = panel->branch_selected - create;
  if (index < 0 || index >= panel->repo.branch_count || !saved(e))
    return;
  const git_branch *branch = &panel->repo.branches[index];
  const char *name = branch->name;
  int track = branch->remote;
  if (track && panel->mode == GIT_PICK_CHECKOUT) {
    const char *local = strchr(name, '/');
    for (int i = 0; local && i < panel->repo.branch_count; i++)
      if (!panel->repo.branches[i].remote && !strcmp(panel->repo.branches[i].name, local + 1)) {
        name = panel->repo.branches[i].name;
        track = 0;
        break;
      }
  }
  const char *args[6];
  int length = 0;
  if (panel->mode == GIT_PICK_MERGE) {
    args[length++] = "merge";
    args[length++] = "--no-edit";
  } else {
    args[length++] = "switch";
    if (track)
      args[length++] = "--track";
  }
  args[length++] = "--";
  args[length++] = name;
  args[length] = NULL;
  git_action action = panel->mode == GIT_PICK_MERGE ? GIT_MERGE : GIT_CHECKOUT;
  panel->mode = GIT_FILES;
  start(e, action, panel->repo.root, args);
}

static void input_key(editor *e, int key) {
  git_panel *panel = &e->git;
  char *input = panel->input;
  int cursor = panel->input_cursor, length = panel->input_length;
  if (key == CTRL('g') || key == '\x1b') {
    panel->mode = GIT_FILES;
    return;
  }
  if (key == '\r' || key == '\n') {
    if (!length || strspn(input, " \t") == (size_t)length) {
      message(panel, 1, "Enter a message or branch name");
      return;
    }
    if (panel->mode == GIT_MESSAGE) {
      const char *args[] = {"commit", "--message", input, NULL};
      if (start(e, GIT_COMMIT, panel->repo.root, args) == 0)
        panel->mode = GIT_FILES;
    } else if (saved(e)) {
      const char *args[] = {"switch", "-c", input, "--", NULL};
      if (start(e, GIT_CREATE_BRANCH, panel->repo.root, args) == 0)
        panel->mode = GIT_FILES;
    }
    return;
  }
  if (key == KEY_LEFT || key == CTRL('b'))
    panel->input_cursor = grapheme_prev(input, cursor);
  else if (key == KEY_RIGHT || key == CTRL('f'))
    panel->input_cursor = cursor < length ? grapheme_next(input, cursor) : length;
  else if (key == KEY_HOME || key == CTRL('a'))
    panel->input_cursor = 0;
  else if (key == KEY_END || key == CTRL('e'))
    panel->input_cursor = length;
  else if ((key == KEY_BACKSPACE || key == CTRL('h')) && cursor > 0) {
    int start = grapheme_prev(input, cursor);
    memmove(input + start, input + cursor, (size_t)(length - cursor + 1));
    panel->input_length -= cursor - start;
    panel->input_cursor = start;
  } else if ((key == KEY_DELETE || key == CTRL('d')) && cursor < length) {
    int end = grapheme_next(input, cursor);
    memmove(input + cursor, input + end, (size_t)(length - end + 1));
    panel->input_length -= end - cursor;
  } else if (key >= 32 && key < KEY_SPECIAL && key != KEY_BACKSPACE) {
    char bytes[4];
    int count = utf8_encode(key, bytes);
    if (count && length + count < (int)sizeof panel->input) {
      memmove(input + cursor + count, input + cursor, (size_t)(length - cursor + 1));
      memcpy(input + cursor, bytes, (size_t)count);
      panel->input_length += count;
      panel->input_cursor += count;
    }
  }
}

void git_panel_key(editor *e, int key) {
  git_panel *panel = &e->git;
  if (panel->action != GIT_IDLE) {
    if (key == CTRL('g') || key == '\x1b')
      e->sidebar.focused = 0;
    return;
  }
  if (git_panel_prompt(e)) {
    input_key(e, key);
    return;
  }
  if (key == CTRL('g') || key == '\x1b') {
    if (panel->mode != GIT_FILES)
      panel->mode = GIT_FILES;
    else
      e->sidebar.focused = 0;
    return;
  }
  int picking = panel->mode == GIT_PICK_CHECKOUT || panel->mode == GIT_PICK_MERGE;
  int count = picking ? panel->repo.branch_count + (panel->mode == GIT_PICK_CHECKOUT) :
                       git_repository_items(&panel->repo);
  int *selected = picking ? &panel->branch_selected : &panel->selected;
  int page = git_panel_list_height(e);
  if (key == KEY_UP || key == CTRL('p')) { if (*selected > 0) (*selected)--; return; }
  if (key == KEY_DOWN || key == CTRL('n')) { if (*selected + 1 < count) (*selected)++; return; }
  if (key == KEY_HOME || key == META('<')) { *selected = 0; return; }
  if (key == KEY_END || key == META('>')) { *selected = count ? count - 1 : 0; return; }
  if (key == KEY_PGUP || key == META('v')) { *selected = *selected > page ? *selected - page : 0; return; }
  if (key == KEY_PGDOWN || key == CTRL('v')) { *selected += page; if (*selected >= count) *selected = count ? count - 1 : 0; return; }
  if (picking) {
    if (key == '\r' || key == '\n')
      activate_branch(e);
    return;
  }
  if (key == 'r') { message(panel, 0, "Git uses saved files"); refresh(e); return; }
  if (!panel->repo.root) { message(panel, 1, "Not in a Git repository; r retries"); return; }
  switch (key) {
  case '\r': case '\n': case 'd': selected_file(e, 1); break;
  case 'o': selected_file(e, 0); break;
  case 's': stage(e, 0, 0); break;
  case 'u': stage(e, 1, 0); break;
  case 'S': stage(e, 0, 1); break;
  case 'U': stage(e, 1, 1); break;
  case 'c':
    if (panel->repo.conflicts)
      message(panel, 1, "Resolve and stage conflicts first");
    else if (!panel->repo.staged)
      message(panel, 1, "Stage changes before committing");
    else
      begin_input(panel, GIT_MESSAGE);
    break;
  case 'p':
    if (saved(e)) {
      const char *args[] = {"pull", "--ff-only", NULL};
      start(e, GIT_PULL, panel->repo.root, args);
    }
    break;
  case 'P': {
    const char *args[] = {"push", NULL};
    start(e, GIT_PUSH, panel->repo.root, args);
    break;
  }
  case 'b': branches(e, 0); break;
  case 'm': branches(e, 1); break;
  default: break;
  }
}

void git_panel_bindings(void) {
  dispatch_bind_prefix_global('v', toggle, "v", "Toggle Git panel");
}
