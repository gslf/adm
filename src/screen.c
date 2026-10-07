#include "search.h"
#include "screen.h"
#include "cursor.h"
#include "utf8.h"
#include "path.h"
#include "syntax.h"
#include "lsp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define WRITE _write
#else
#include <unistd.h>
#define WRITE write
#endif

#define SELECT_ON "\x1b[44;97m"
#define STATUS_COLOURS "\x1b[30;103m"
#define WARNING_COLOURS "\x1b[97;41m"

void ab_append(abuf *ab, const char *s, int len) {
  if (len <= 0)
    return;
  char *nb = realloc(ab->b, ab->len + len);
  if (!nb)
    return;
  memcpy(nb + ab->len, s, len);
  ab->b = nb;
  ab->len += len;
}

void ab_free(abuf *ab) {
  free(ab->b);
  ab->b = NULL;
  ab->len = 0;
}

static void append_str(abuf *ab, const char *s) {
  ab_append(ab, s, (int)strlen(s));
}

void screen_position(abuf *ab, int x, int y) {
  char sequence[40];
  int n = snprintf(sequence, sizeof sequence, "\x1b[%d;%dH", y + 1, x + 1);
  ab_append(ab, sequence, n);
}

void screen_repeat(abuf *ab, char c, int count) {
  char chunk[64];
  memset(chunk, c, sizeof chunk);
  while (count > 0) {
    int n = count < (int)sizeof chunk ? count : (int)sizeof chunk;
    ab_append(ab, chunk, n);
    count -= n;
  }
}

void screen_fill(abuf *ab, rect area, char c) {
  for (int y = 0; y < area.height; y++) {
    screen_position(ab, area.x, area.y + y);
    screen_repeat(ab, c, area.width);
  }
}

int screen_text(abuf *ab, const char *text, int width) {
  int used = 0;
  for (int j = 0; text[j]; ) {
    int control = (unsigned char)text[j] < 32 || text[j] == 127;
    int w = control ? 1 : grapheme_width(text, j);
    int next = control ? j + 1 : grapheme_next(text, j);
    if (used + w > width || next <= j)
      break;
    ab_append(ab, control ? "?" : text + j, control ? 1 : next - j);
    used += w;
    j = next;
  }
  return used;
}

int screen_gutter(const view *v) {
  if (!v->doc)
    return 0;
  int n = v->doc->buf.nlines, digits = 1;
  while (n >= 10) {
    n /= 10;
    digits++;
  }
  return digits + 1;
}

static void draw_top(const editor *e, abuf *ab) {
  char text[512];
  const document *doc = e->view->doc;
  screen_position(ab, 0, 0);
  append_str(ab, STATUS_COLOURS);
  append_str(ab, "\x1b[1m");
  int used = screen_text(ab, " ][adm", e->cols);
  append_str(ab, "\x1b[22m");
  if (e->tab_count > 1) {
    tabs_draw(e, ab, e->cols - used);
    append_str(ab, "\x1b[m");
    return;
  }
  const char *name = !doc ? "[Empty]" : doc->label ? doc->label :
                     doc->filename ? path_name(doc->filename) : document_name(doc);
  snprintf(text, sizeof text, "  %s%s", name,
           doc && doc->dirty ? " **" : "");
  used += screen_text(ab, text, e->cols - used);
  screen_repeat(ab, ' ', e->cols - used);
  append_str(ab, "\x1b[m");
}

static const char *row_colours(const document *doc, int row) {
  if (!doc->diff_lines)
    return "\x1b[m";
  switch (doc->diff_lines[row]) {
  case DIFF_ADDED: return "\x1b[0;38;5;194;48;5;22m";
  case DIFF_REMOVED: return "\x1b[0;38;5;224;48;5;52m";
  case DIFF_CONTEXT: return "\x1b[0;38;5;252;48;5;235m";
  case DIFF_HUNK: return "\x1b[0;36m";
  default: return "\x1b[0;90m";
  }
}

static void draw_row(const view *v, abuf *ab, rect area, int y, int highlight) {
  int row = v->rowoff + y;
  screen_position(ab, area.x, area.y + y);
  if (row >= v->doc->buf.nlines) {
    ab_append(ab, "~", area.width > 0 ? 1 : 0);
    return;
  }
  const char *colours = row_colours(v->doc, row);
  if (v->doc->diff_lines) {
    append_str(ab, colours);
    screen_repeat(ab, ' ', area.width);
    screen_position(ab, area.x, area.y + y);
  }
  int gutter = screen_gutter(v);
  char number[32];
  int digits = gutter - 1 < 11 ? gutter - 1 : 11;
  snprintf(number, sizeof number, "%*d ", digits, row + 1);
  screen_text(ab, number, area.width);
  int width = area.width - gutter;
  if (width <= 0) {
    append_str(ab, "\x1b[m");
    return;
  }

  const char *line = buffer_line(&v->doc->buf, row);
  int len = line ? (int)strlen(line) : 0;
  int sr, sc, er, ec, from = 0, to = 0;
  if (view_selection_range(v, &sr, &sc, &er, &ec) && row >= sr && row <= er) {
    from = row == sr ? sc : 0;
    to = row == er ? ec : len + 1;
  }
  syntax_span spans[256];
  size_t span_count = highlight ? syntax_line(v->doc, row, spans, 256) : 0, span_index = 0;
  syntax_kind previous_kind = SYNTAX_TEXT;
  int painted = 0, col = 0;
  for (int j = 0; ; ) {
    int inside = j >= from && j < to;
    int at_end = j >= len;
    if (at_end && !inside)
      break;
    int w = at_end ? 1 : grapheme_width(line, j);
    if (col + w > v->coloff + width)
      break;
    if (col >= v->coloff || col + w > v->coloff) {
      while (span_index < span_count && j >= spans[span_index].end) span_index++;
      syntax_kind kind = span_index < span_count && j >= spans[span_index].start
                           ? spans[span_index].kind : SYNTAX_TEXT;
      if (inside != painted || (!inside && kind != previous_kind)) {
        append_str(ab, inside ? SELECT_ON : colours);
        if (!inside && !v->doc->diff_lines) append_str(ab, syntax_colour(kind));
        painted = inside;
        previous_kind = kind;
      }
      if (at_end || col < v->coloff)
        ab_append(ab, " ", 1);
      else
        ab_append(ab, line + j, grapheme_next(line, j) - j);
    }
    col += w;
    if (at_end)
      break;
    int next = grapheme_next(line, j);
    if (next <= j)
      break;
    j = next;
  }
  if (painted || v->doc->diff_lines || span_count)
    append_str(ab, "\x1b[m");
}

void screen_empty(abuf *ab, rect area) {
  if (area.width <= 0 || area.height <= 0)
    return;
  static const char *mark[] = {
    "____   ____", "    | |    ", "    | |    ", "    | |    ", "____| |____"
  };
  static const char *name[] = {
    "           _           ", "  __ _  __| |_ __ ___  ",
    " / _` |/ _` | '_ ` _ \\ ", "| (_| | (_| | | | | | |",
    " \\__,_|\\__,_|_| |_| |_|"
  };
  int large = area.width >= 36 && area.height >= 7;
  int height = large ? 5 : 1, width = large ? 36 : 7;
  int x = area.x + (area.width > width ? (area.width - width) / 2 : 0);
  int y = area.y + (area.height > height + 2 ? (area.height - height - 2) / 2 : 0);
  for (int row = 0; row < height; row++) {
    screen_position(ab, x, y + row);
    append_str(ab, "\x1b[1;93m");
    int used = screen_text(ab, large ? mark[row] : "][", area.x + area.width - x);
    append_str(ab, "\x1b[m");
    used += screen_text(ab, large ? "  " : " ", area.x + area.width - x - used);
    screen_text(ab, large ? name[row] : "adm", area.x + area.width - x - used);
  }
  if (y + height + 1 < area.y + area.height) {
    const char *hint = "C-x N: Create new file";
    int length = (int)strlen(hint);
    int left = area.x + (area.width > length ? (area.width - length) / 2 : 0);
    screen_position(ab, left, y + height + 1);
    screen_text(ab, hint, area.x + area.width - left);
  }
}

static void draw_pane(editor *e, abuf *ab, view *v, int ordinal) {
  if (v->area.width <= 0 || v->area.height <= 0)
    return;
  if (e->windows.count > 1) {
    char title[512];
    snprintf(title, sizeof title, " %c %d  %s%s", v == e->view ? '*' : ' ', ordinal,
             !v->doc ? "[Empty]" : v->doc->label ? v->doc->label : v->doc->filename ? path_name(v->doc->filename) : "[No Name]",
             v->doc && v->doc->dirty ? " **" : "");
    screen_position(ab, v->area.x, v->area.y);
    append_str(ab, v == e->view ? "\x1b[97;44m" : "\x1b[30;47m");
    int used = screen_text(ab, title, v->area.width);
    screen_repeat(ab, ' ', v->area.width - used);
    append_str(ab, "\x1b[m");
  }
  rect area = layout_content(e, v);
  if (!v->doc) {
    screen_empty(ab, area);
    return;
  }
  cursor_scroll_view(v, area.width - screen_gutter(v), area.height);
  if (e->syntax_enabled) syntax_prepare(v->doc, v->rowoff + area.height);
  for (int y = 0; y < area.height; y++)
    draw_row(v, ab, area, y, e->syntax_enabled);
}

static void draw_workspace(editor *e, abuf *ab) {
  screen_fill(ab, (rect){0, 1, e->cols, e->rows > 2 ? e->rows - 2 : 0}, ' ');
  int panes[MAX_PANES], count = layout_order(e, panes);
  for (int i = 0; i < count; i++)
    draw_pane(e, ab, &e->windows.panes[panes[i]], i + 1);
  if (e->windows.compact)
    return;
  append_str(ab, "\x1b[90m");
  for (int i = 0; i < MAX_LAYOUT_NODES; i++) {
    const layout_node *n = &e->windows.nodes[i];
    if (!n->used || n->kind == LAYOUT_LEAF)
      continue;
    rect separator = n->area;
    const rect first = e->windows.nodes[n->first].area;
    if (n->kind == LAYOUT_VERTICAL) {
      separator.x += first.width;
      separator.width = 1;
      screen_fill(ab, separator, '|');
    } else {
      separator.y += first.height;
      separator.height = 1;
      screen_fill(ab, separator, '-');
    }
  }
  append_str(ab, "\x1b[m");
}

static void draw_file_manager(editor *e, abuf *ab) {
  rect area = file_manager_area(e);
  if (area.width <= 0)
    return;
  if (search_files_draw(e, ab, area))
    return;
  file_tree *tree = &e->files.tree;
  screen_position(ab, area.x, area.y);
  append_str(ab, e->sidebar.focused ? "\x1b[97;44m" : "\x1b[30;47m");
  int used = screen_text(ab, e->sidebar.focused ? " FILES *" : " FILES", area.width);
  screen_repeat(ab, ' ', area.width - used);
  append_str(ab, "\x1b[m");
  int height = area.height - 2;
  file_tree_scroll(tree, height);
  for (int row = 0; row < height && tree->offset + row < tree->count; row++) {
    int index = tree->offset + row;
    const tree_entry *entry = &tree->entries[index];
    screen_position(ab, area.x, area.y + row + 1);
    if (index == tree->selected)
      append_str(ab, e->sidebar.focused ? "\x1b[97;44m" : "\x1b[47;30m");
    int indent = entry->depth < area.width / 2 ? entry->depth * 2 : area.width - 2;
    screen_repeat(ab, ' ', indent);
    used = indent;
    used += screen_text(ab, entry->directory ? entry->expanded ? "v " : "> " : "  ",
                         area.width - used);
    used += screen_text(ab, path_name(entry->path), area.width - used);
    screen_repeat(ab, ' ', area.width - used);
    append_str(ab, "\x1b[m");
  }
  screen_position(ab, area.x, area.y + area.height - 1);
  append_str(ab, tree->error[0] ? WARNING_COLOURS : "\x1b[90m");
  screen_text(ab, tree->error[0] ? tree->error : "Enter: open  C-l: editor", area.width);
  append_str(ab, "\x1b[90m");
  screen_fill(ab, (rect){area.width, area.y, 1, area.height}, '|');
  append_str(ab, "\x1b[m");
}

static void draw_bottom(editor *e, abuf *ab) {
  screen_position(ab, 0, e->rows - 1);
  char text[512];
  int used = 0;
  if (e->confirmation) {
    append_str(ab, WARNING_COLOURS);
    snprintf(text, sizeof text, " %s y=yes / any=no", e->confirmation_prompt);
  } else if (e->move_active) {
    append_str(ab, STATUS_COLOURS);
    snprintf(text, sizeof text, "%s", e->cols >= 50 ?
        " MOVE h/j/k/l: split   n/p: tab   b: sidebar   Esc" :
        e->cols >= 40 ? " MOVE hjkl:split n/p:tab  b:sidebar  Esc" :
        e->cols >= 32 ? " MOVE hjkl:split np:tab b Esc" :
        " MOVE hjkl np b Esc");
  } else if (e->help_active) {
    append_str(ab, STATUS_COLOURS);
    snprintf(text, sizeof text, " HELP  Up/Down: scroll  ESC: close");
  } else if (e->notice[0]) {
    append_str(ab, WARNING_COLOURS);
    snprintf(text, sizeof text, " %s", e->notice);
  } else if (!e->view->doc) {
    append_str(ab, STATUS_COLOURS);
    snprintf(text, sizeof text, " [C-x]  Select a file  |  C-x N: Create new file");
  } else {
    append_str(ab, STATUS_COLOURS);
    used = screen_text(ab, " [C-x] ", e->cols);
    if (e->view->doc->readonly)
      used += screen_text(ab, " DIFF ", e->cols - used);
    if ((e->view->sel_mode || e->view->sel_active) && e->cols - used >= 8) {
      append_str(ab, "\x1b[97;44;1m SELECT \x1b[22m");
      append_str(ab, STATUS_COLOURS);
      used += 8;
    }
    int length = snprintf(text, sizeof text, " %d:%d  lines %d  chars %zu",
        e->view->cy + 1, cursor_col(e) + 1, e->view->doc->buf.nlines,
        buffer_char_count(&e->view->doc->buf));
    if (e->view->sel_active && length > 0 && length < (int)sizeof text)
      length += snprintf(text + length, sizeof text - length, "  sel %d",
                         selection_char_count(e));
    if (e->windows.count > 1 && length > 0 && length < (int)sizeof text) {
      int panes[MAX_PANES], count = layout_order(e, panes), index = 0;
      while (panes[index] != e->windows.active)
        index++;
      snprintf(text + length, sizeof text - length, "  split %d/%d%s", index + 1,
               count, e->windows.compact ? " compact" : "");
    }
  }
  used += screen_text(ab, text, e->cols - used);
  screen_repeat(ab, ' ', e->cols - used);
  append_str(ab, "\x1b[m");
}

void screen_refresh(editor *e) {
  layout_arrange(e);
  abuf ab = {0};
  append_str(&ab, "\x1b[?25l");
  draw_top(e, &ab);
  draw_workspace(e, &ab);
  draw_file_manager(e, &ab);
  git_panel_draw(e, &ab);
  draw_bottom(e, &ab);
  dispatch_draw(e, &ab);
  rect area = layout_content(e, e->view);
  int gutter = screen_gutter(e->view);
  int prompt_x, prompt_y;
  if (lsp_cursor(e, &prompt_x, &prompt_y)) {
    screen_position(&ab, prompt_x, prompt_y);
    append_str(&ab, "\x1b[?25h");
  }
  if (!e->move_active && !e->help_active && !e->prefix_active && !lsp_modal(e) && !e->confirmation &&
      (search_cursor(e, &prompt_x, &prompt_y) || new_file_cursor(e, &prompt_x, &prompt_y) || git_panel_cursor(e, &prompt_x, &prompt_y))) {
    screen_position(&ab, prompt_x, prompt_y);
    append_str(&ab, "\x1b[?25h");
  }
  if (e->view->doc && !search_active(e) && !e->move_active && !e->help_active && !e->new_file.active && !e->prefix_active && !lsp_modal(e) && !e->confirmation && !e->sidebar.focused &&
      area.width > gutter && area.height > 0) {
    int x = area.x + gutter + cursor_col(e) - e->view->coloff;
    int y = area.y + e->view->cy - e->view->rowoff;
    screen_position(&ab, x, y);
    append_str(&ab, "\x1b[?25h");
  }
  WRITE(1, ab.b, ab.len);
  ab_free(&ab);
}

void screen_clear(void) {
  WRITE(1, "\x1b[2J\x1b[H", 7);
}
