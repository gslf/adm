#include "git_panel.h"
#include "screen.h"
#include "utf8.h"

#include <stdio.h>
#include <string.h>

static void style(abuf *ab, const char *code) {
  ab_append(ab, code, (int)strlen(code));
}

static void line(abuf *ab, rect area, int row, const char *text, const char *colour) {
  if (row < 0 || row >= area.height)
    return;
  screen_position(ab, area.x, area.y + row);
  style(ab, colour);
  int used = screen_text(ab, text, area.width);
  screen_repeat(ab, ' ', area.width - used);
  style(ab, "\x1b[m");
}

static int selected_colour(const editor *e) {
  return e->sidebar.focused;
}

static void scroll(int *offset, int selected, int count, int height) {
  if (selected < *offset)
    *offset = selected;
  if (selected >= *offset + height)
    *offset = selected - height + 1;
  int last = count > height ? count - height : 0;
  if (*offset > last)
    *offset = last;
  if (*offset < 0)
    *offset = 0;
}

static void files(editor *e, abuf *ab, rect area, int height) {
  git_panel *panel = &e->git;
  git_repository *repo = &panel->repo;
  int total = git_repository_items(repo) + 2 + (repo->conflicts > 0);
  scroll(&panel->offset, git_panel_selected_row(panel), total, height);
  int logical = 0, item = 0;
  for (int section = GIT_CONFLICT; section <= GIT_UNSTAGED; section++) {
    int count = section == GIT_CONFLICT ? repo->conflicts :
                section == GIT_STAGED ? repo->staged : repo->unstaged;
    if (section == GIT_CONFLICT && !count)
      continue;
    char header[80];
    snprintf(header, sizeof header, " %s (%d)", section == GIT_CONFLICT ? "Conflicts" :
             section == GIT_STAGED ? "Staged" : "Changes", count);
    int row = logical++ - panel->offset;
    if (row >= 0 && row < height)
      line(ab, area, row + 2, header, "\x1b[1;36m");
    for (int i = 0; i < count; i++, item++) {
      git_item entry = git_repository_item(repo, item);
      row = logical++ - panel->offset;
      if (row < 0 || row >= height)
        continue;
      const git_file *file = entry.file;
      screen_position(ab, area.x, area.y + row + 2);
      style(ab, item == panel->selected ? selected_colour(e) ? "\x1b[97;44m" : "\x1b[30;47m" : "\x1b[m");
      char status[8];
      snprintf(status, sizeof status, " %c ", file->conflict ? '!' : file->untracked ? '?' :
               section == GIT_STAGED ? file->index : file->worktree);
      int used = screen_text(ab, status, area.width);
      used += screen_text(ab, file->path, area.width - used);
      if (file->original) {
        used += screen_text(ab, " <- ", area.width - used);
        used += screen_text(ab, file->original, area.width - used);
      }
      screen_repeat(ab, ' ', area.width - used);
      style(ab, "\x1b[m");
    }
  }
  if (!repo->count && height > 2)
    line(ab, area, 4, repo->root ? " Working tree clean" : " No repository loaded", "\x1b[90m");
}

static void branches(editor *e, abuf *ab, rect area, int height) {
  git_panel *panel = &e->git;
  int create = panel->mode == GIT_PICK_CHECKOUT;
  int count = panel->repo.branch_count + create;
  scroll(&panel->branch_offset, panel->branch_selected, count, height);
  for (int row = 0; row < height && row + panel->branch_offset < count; row++) {
    int index = row + panel->branch_offset;
    char text[1024];
    if (create && !index)
      snprintf(text, sizeof text, " + New branch...");
    else {
      const git_branch *branch = &panel->repo.branches[index - create];
      snprintf(text, sizeof text, " %s%s", branch->name, branch->remote ? " [remote]" : "");
    }
    line(ab, area, row + 2, text, index == panel->branch_selected ?
          selected_colour(e) ? "\x1b[97;44m" : "\x1b[30;47m" : "\x1b[m");
  }
}

static int input_column(const git_panel *panel) {
  int width = 0;
  for (int i = panel->input_offset; i < panel->input_cursor; i = grapheme_next(panel->input, i))
    width += grapheme_width(panel->input, i);
  return width;
}

static void footer(editor *e, abuf *ab, rect area) {
  git_panel *panel = &e->git;
  int height = git_panel_footer_height(e), start = area.height - height;
  const char *colour = panel->failed ? "\x1b[97;41m" : "\x1b[90m";
  if (panel->mode == GIT_MESSAGE || panel->mode == GIT_NEW_BRANCH) {
    if (panel->input_offset > panel->input_cursor)
      panel->input_offset = panel->input_cursor;
    while (input_column(panel) >= area.width - 1 && panel->input_offset < panel->input_cursor)
      panel->input_offset = grapheme_next(panel->input, panel->input_offset);
    line(ab, area, start, panel->mode == GIT_MESSAGE ? " Commit message:" : " New branch name:", "\x1b[1m");
    line(ab, area, start + 1, panel->input + panel->input_offset, "\x1b[30;47m");
    line(ab, area, start + 2, "Enter confirm  C-g cancel", "\x1b[90m");
    line(ab, area, start + 3, panel->message, colour);
  } else if (panel->mode != GIT_FILES) {
    line(ab, area, start, panel->mode == GIT_PICK_MERGE ? "Enter merge  C-g back" : "Enter checkout C-g back", "\x1b[90m");
    line(ab, area, start + 1, "Up/Down choose branch", "\x1b[90m");
    line(ab, area, start + 2, panel->message, colour);
  } else {
    static const char *const bindings[] = {
      "Enter diff   o open", "s/u stage/unstage", "S/U stage/unstage all",
      "c commit    r refresh", "p/P pull/push", "b checkout  m merge", "C-l editor"
    };
    line(ab, area, start, panel->message, colour);
    for (int i = 1; i < height; i++)
      line(ab, area, start + i, bindings[i - 1], "\x1b[90m");
  }
}

void git_panel_draw(editor *e, abuf *ab) {
  if (e->sidebar.kind != SIDEBAR_GIT)
    return;
  rect area = sidebar_area(e);
  if (area.width <= 0)
    return;
  git_panel *panel = &e->git;
  char title[96];
  snprintf(title, sizeof title, " GIT%s%s", e->sidebar.focused ? " *" : "",
           panel->action != GIT_IDLE ? "  working..." : "");
  line(ab, area, 0, title, e->sidebar.focused ? "\x1b[97;44m" : "\x1b[30;47m");
  line(ab, area, 1, panel->repo.head ? panel->repo.head : "Repository status", "\x1b[36m");
  int height = git_panel_list_height(e);
  if (panel->mode == GIT_PICK_CHECKOUT || panel->mode == GIT_PICK_MERGE)
    branches(e, ab, area, height);
  else
    files(e, ab, area, height);
  footer(e, ab, area);
  style(ab, "\x1b[90m");
  screen_fill(ab, (rect){area.width, area.y, 1, area.height}, '|');
  style(ab, "\x1b[m");
}

int git_panel_cursor(const editor *e, int *x, int *y) {
  if (e->sidebar.kind != SIDEBAR_GIT || !git_panel_prompt(e) || e->git.action != GIT_IDLE)
    return 0;
  rect area = sidebar_area(e);
  if (!area.width || git_panel_footer_height(e) < 2)
    return 0;
  *x = area.x + input_column(&e->git);
  *y = area.y + area.height - git_panel_footer_height(e) + 1;
  return 1;
}
