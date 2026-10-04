#include "screen.h"
#include "cursor.h"
#include "utf8.h"
#include "path.h"

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
#define SELECT_OFF "\x1b[49;39m"
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

static void position(abuf *ab, int x, int y) {
  char sequence[40];
  int n = snprintf(sequence, sizeof sequence, "\x1b[%d;%dH", y + 1, x + 1);
  ab_append(ab, sequence, n);
}

static void repeat(abuf *ab, char c, int count) {
  char chunk[64];
  memset(chunk, c, sizeof chunk);
  while (count > 0) {
    int n = count < (int)sizeof chunk ? count : (int)sizeof chunk;
    ab_append(ab, chunk, n);
    count -= n;
  }
}

static void fill(abuf *ab, rect area, char c) {
  for (int y = 0; y < area.height; y++) {
    position(ab, area.x, area.y + y);
    repeat(ab, c, area.width);
  }
}

static int clipped_text(abuf *ab, const char *text, int width) {
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
  position(ab, 0, 0);
  append_str(ab, STATUS_COLOURS);
  append_str(ab, "\x1b[1m");
  int used = clipped_text(ab, " ][adm", e->cols);
  append_str(ab, "\x1b[22m");
  snprintf(text, sizeof text, "  %s%s", doc->filename ? doc->filename : "[No Name]",
           doc->dirty ? " **" : "");
  used += clipped_text(ab, text, e->cols - used);
  repeat(ab, ' ', e->cols - used);
  append_str(ab, "\x1b[m");
}

static void draw_row(const view *v, abuf *ab, rect area, int y) {
  int row = v->rowoff + y;
  position(ab, area.x, area.y + y);
  if (row >= v->doc->buf.nlines) {
    ab_append(ab, "~", area.width > 0 ? 1 : 0);
    return;
  }
  int gutter = screen_gutter(v);
  char number[32];
  int digits = gutter - 1 < 11 ? gutter - 1 : 11;
  snprintf(number, sizeof number, "%*d ", digits, row + 1);
  clipped_text(ab, number, area.width);
  int width = area.width - gutter;
  if (width <= 0)
    return;

  const char *line = buffer_line(&v->doc->buf, row);
  int len = line ? (int)strlen(line) : 0;
  int sr, sc, er, ec, from = 0, to = 0;
  if (view_selection_range(v, &sr, &sc, &er, &ec) && row >= sr && row <= er) {
    from = row == sr ? sc : 0;
    to = row == er ? ec : len + 1;
  }
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
      if (inside != painted) {
        append_str(ab, inside ? SELECT_ON : SELECT_OFF);
        painted = inside;
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
  if (painted)
    append_str(ab, SELECT_OFF);
}

static void draw_pane(editor *e, abuf *ab, view *v, int ordinal) {
  if (v->area.width <= 0 || v->area.height <= 0)
    return;
  if (e->windows.count > 1) {
    char title[512];
    snprintf(title, sizeof title, " %c %d  %s%s", v == e->view ? '*' : ' ', ordinal,
             v->doc->filename ? path_name(v->doc->filename) : "[No Name]",
             v->doc->dirty ? " **" : "");
    position(ab, v->area.x, v->area.y);
    append_str(ab, v == e->view ? "\x1b[97;44m" : "\x1b[30;47m");
    int used = clipped_text(ab, title, v->area.width);
    repeat(ab, ' ', v->area.width - used);
    append_str(ab, "\x1b[m");
  }
  rect area = layout_content(e, v);
  cursor_scroll_view(v, area.width - screen_gutter(v), area.height);
  for (int y = 0; y < area.height; y++)
    draw_row(v, ab, area, y);
}

static void draw_workspace(editor *e, abuf *ab) {
  fill(ab, (rect){0, 1, e->cols, e->rows > 2 ? e->rows - 2 : 0}, ' ');
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
      fill(ab, separator, '|');
    } else {
      separator.y += first.height;
      separator.height = 1;
      fill(ab, separator, '-');
    }
  }
  append_str(ab, "\x1b[m");
}

static void draw_file_manager(editor *e, abuf *ab) {
  rect area = file_manager_area(e);
  if (area.width <= 0)
    return;
  file_tree *tree = &e->files.tree;
  position(ab, area.x, area.y);
  append_str(ab, e->files.focused ? "\x1b[97;44m" : "\x1b[30;47m");
  int used = clipped_text(ab, e->files.focused ? " FILES *" : " FILES", area.width);
  repeat(ab, ' ', area.width - used);
  append_str(ab, "\x1b[m");
  int height = area.height - 2;
  file_tree_scroll(tree, height);
  for (int row = 0; row < height && tree->offset + row < tree->count; row++) {
    int index = tree->offset + row;
    const tree_entry *entry = &tree->entries[index];
    position(ab, area.x, area.y + row + 1);
    if (index == tree->selected)
      append_str(ab, e->files.focused ? "\x1b[97;44m" : "\x1b[47;30m");
    int indent = entry->depth < area.width / 2 ? entry->depth * 2 : area.width - 2;
    repeat(ab, ' ', indent);
    used = indent;
    used += clipped_text(ab, entry->directory ? entry->expanded ? "v " : "> " : "  ",
                         area.width - used);
    used += clipped_text(ab, path_name(entry->path), area.width - used);
    repeat(ab, ' ', area.width - used);
    append_str(ab, "\x1b[m");
  }
  position(ab, area.x, area.y + area.height - 1);
  append_str(ab, tree->error[0] ? WARNING_COLOURS : "\x1b[90m");
  clipped_text(ab, tree->error[0] ? tree->error : "Enter: open  C-g: editor", area.width);
  append_str(ab, "\x1b[90m");
  fill(ab, (rect){area.width, area.y, 1, area.height}, '|');
  append_str(ab, "\x1b[m");
}

static void draw_bottom(const editor *e, abuf *ab) {
  position(ab, 0, e->rows - 1);
  char text[512];
  int used = 0;
  if (e->confirmation) {
    append_str(ab, WARNING_COLOURS);
    snprintf(text, sizeof text, " %s y=yes / any=no", e->confirmation_prompt);
  } else {
    append_str(ab, STATUS_COLOURS);
    used = clipped_text(ab, " [C-x] ", e->cols);
    if ((e->view->sel_mode || e->view->sel_active) && e->cols - used >= 8) {
      append_str(ab, "\x1b[97;44;1m SELECT \x1b[22m");
      append_str(ab, STATUS_COLOURS);
      used += 8;
    }
    int length = snprintf(text, sizeof text, " %d:%d  lines %d  chars %d",
        e->view->cy + 1, cursor_col(e) + 1, e->view->doc->buf.nlines,
        buffer_char_count(&e->view->doc->buf));
    if (e->view->sel_active && length > 0 && length < (int)sizeof text)
      length += snprintf(text + length, sizeof text - length, "  sel %d",
                         selection_char_count(e));
    if (e->windows.count > 1 && length > 0 && length < (int)sizeof text) {
      int panes[MAX_PANES], count = layout_order(e, panes), index = 0;
      while (panes[index] != e->windows.active)
        index++;
      snprintf(text + length, sizeof text - length, "  pane %d/%d%s", index + 1,
               count, e->windows.compact ? " compact" : "");
    }
  }
  used += clipped_text(ab, text, e->cols - used);
  repeat(ab, ' ', e->cols - used);
  append_str(ab, "\x1b[m");
}

void screen_refresh(editor *e) {
  layout_arrange(e);
  abuf ab = {0};
  append_str(&ab, "\x1b[?25l");
  draw_top(e, &ab);
  draw_workspace(e, &ab);
  draw_file_manager(e, &ab);
  draw_bottom(e, &ab);
  dispatch_draw(e, &ab);
  rect area = layout_content(e, e->view);
  int gutter = screen_gutter(e->view);
  if (!e->prefix_active && !e->confirmation && !e->files.focused &&
      area.width > gutter && area.height > 0) {
    int x = area.x + gutter + cursor_col(e) - e->view->coloff;
    int y = area.y + e->view->cy - e->view->rowoff;
    position(&ab, x, y);
    append_str(&ab, "\x1b[?25h");
  }
  WRITE(1, ab.b, ab.len);
  ab_free(&ab);
}

void screen_clear(void) {
  WRITE(1, "\x1b[2J\x1b[H", 7);
}
