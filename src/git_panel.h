#ifndef GIT_PANEL_H
#define GIT_PANEL_H

#include "git_repository.h"
#include "process.h"
#include "view.h"

typedef enum {
  GIT_IDLE, GIT_DISCOVER, GIT_STATUS, GIT_DIFF, GIT_STAGE, GIT_UNSTAGE,
  GIT_STAGE_ALL, GIT_UNSTAGE_ALL, GIT_COMMIT, GIT_PULL, GIT_PUSH,
  GIT_CHECKOUT, GIT_CREATE_BRANCH, GIT_MERGE, GIT_BRANCHES
} git_action;

typedef enum { GIT_FILES, GIT_PICK_CHECKOUT, GIT_PICK_MERGE, GIT_MESSAGE, GIT_NEW_BRANCH } git_mode;

typedef struct git_panel {
  git_repository repo;
  child_process process;
  git_action action;
  git_mode mode;
  int selected, offset, branch_selected, branch_offset;
  int failed, diff_untracked, diff_pane;
  git_section diff_section;
  unsigned long diff_revision, diff_tab_id;
  char *action_path;
  char input[2048];
  int input_length, input_cursor, input_offset;
  char message[512];
} git_panel;

struct editor;
void git_panel_init(struct editor *e);
void git_panel_shutdown(struct editor *e);
void git_panel_tick(struct editor *e);
void git_panel_key(struct editor *e, int key);
void git_panel_bindings(void);
int git_panel_worktree_busy(const struct editor *e);
int git_panel_prompt(const struct editor *e);
int git_panel_list_height(const struct editor *e);
int git_panel_selected_row(const git_panel *panel);
int git_panel_footer_height(const struct editor *e);
struct abuf;
void git_panel_draw(struct editor *e, struct abuf *ab);
int git_panel_cursor(const struct editor *e, int *x, int *y);
void git_panel_refresh(struct editor *e);

#endif
