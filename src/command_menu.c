#include "theme.h"
#include "command_menu.h"
#include "screen.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

// The Command Center files every C-x command under a heading, the way
// which-key and Magit's transient menus do: a command is found by what it is
// for, not by the order its module happened to register it in. Groups flow
// into as many columns as the terminal has room for, so the usual 80x24
// terminal shows them all at once. Positions never move: a command that cannot
// run right now is dimmed, not hidden.
//
// A group lists the primary key of a pair only; the partner joins its row.
// A command missing from every group still appears, under MORE.

#define END KEY_NONE
#define GROUP_KEYS 8

typedef struct {
  const char *title;
  int keys[GROUP_KEYS]; // END terminated
} menu_group;

static const menu_group groups[] = {
  {"FILE",    {CTRL('s'), 'N', CTRL('c'), END}},
  {"EDIT",    {' ', 'z', 'w', END}},
  {"SEARCH",  {'s', 'S', 'r', 'R', 'l', END}},
  {"SIDEBAR", {'f', 'g', 'n', 'd', END}},
  {"SPLITS",  {'q', 'u', 'c', ']', '}', 'm', END}},
  {"TABS",    {'t', 'k', END}},
};
#define GROUP_COUNT ((int)(sizeof groups / sizeof groups[0]))

// Hints in the bottom bar rather than rows of a group.
static const int footer_keys[] = {'?', END};

#define TITLE "][ adm - Command Center"
#define MORE_TITLE "MORE"

#define MARGIN 1  // inside the box, left and right
#define GUTTER 2  // between columns
#define GAP 2     // between keys, label and shortcut
#define MAX_COLUMNS 3

// Bounded by the key slots; every row holds at least one of them.
#define MENU_ROWS 512

#define BOLD "\x1b[1m"
#define DIM "\x1b[2m"
#define PLAIN "\x1b[22m"

typedef struct {
  const command_binding *first, *second; // second: the pair partner, if any
} menu_row;

typedef struct {
  const char *title;
  int start, count; // rows [start, start + count)
} menu_section;

typedef struct {
  menu_row rows[MENU_ROWS];
  int nrows;
  menu_section sections[GROUP_COUNT + 1];
  int nsections;
  int key_width, label_width, shortcut_width, column_width;
  int columns, shortcuts; // shortcuts: whether the shortcut column fits
  int first[MAX_COLUMNS + 1]; // column c shows sections [first[c], first[c+1])
  int lines;                  // height of the tallest column
} menu_layout;

///////////////////////////////////
// ROWS

// The other half of a pair, as long as both halves still agree on it.
static const command_binding *partner(const command_binding *entry) {
  if (!entry->pair_label)
    return NULL;
  const command_binding *other = dispatch_prefix_lookup(entry->pair_key);
  return other && other->pair_key == entry->key &&
         other->pair_primary == entry->pair_primary && other->pair_label
             ? other : NULL;
}

static int in_footer(int key) {
  for (int i = 0; footer_keys[i] != END; i++)
    if (footer_keys[i] == key)
      return 1;
  return 0;
}

static int placed(const menu_layout *m, const command_binding *entry) {
  for (int i = 0; i < m->nrows; i++)
    if (m->rows[i].first == entry || m->rows[i].second == entry)
      return 1;
  return 0;
}

// A pair always sits on the row of its primary key.
static void add_row(menu_layout *m, const command_binding *entry) {
  if (!entry || in_footer(entry->key) || placed(m, entry) ||
      m->nrows == MENU_ROWS)
    return;
  const command_binding *other = partner(entry);
  if (other && entry->key != entry->pair_primary)
    m->rows[m->nrows++] = (menu_row){other, entry};
  else
    m->rows[m->nrows++] = (menu_row){entry, other};
}

static void add_section(menu_layout *m, const char *title, int start) {
  if (m->nrows > start)
    m->sections[m->nsections++] =
        (menu_section){title, start, m->nrows - start};
}

static void collect(menu_layout *m) {
  m->nrows = m->nsections = 0;
  for (int g = 0; g < GROUP_COUNT; g++) {
    int start = m->nrows;
    for (int k = 0; groups[g].keys[k] != END; k++)
      add_row(m, dispatch_prefix_lookup(groups[g].keys[k]));
    add_section(m, groups[g].title, start);
  }
  int start = m->nrows;
  const command_binding *entry;
  for (int i = 0; (entry = dispatch_prefix_entry(i)); i++)
    add_row(m, entry);
  add_section(m, MORE_TITLE, start);
}

static void row_keys(const menu_row *row, char *text, size_t size) {
  snprintf(text, size, "%s%s%s", row->first->keys, row->second ? "/" : "",
           row->second ? row->second->keys : "");
}

static const char *row_label(const menu_row *row) {
  return row->second ? row->first->pair_label : row->first->label;
}

// The keys that run the command without C-x, like the accelerators a menu bar
// lists next to its items.
static void row_shortcut(const menu_row *row, char *text, size_t size) {
  char first[256], second[256] = "";
  dispatch_direct_bindings(row->first->cmd, first, sizeof first);
  if (row->second)
    dispatch_direct_bindings(row->second->cmd, second, sizeof second);
  snprintf(text, size, "%s%s%s", first, first[0] && second[0] ? "/" : "",
           second);
}

static int row_available(const editor *e, const menu_row *row) {
  return dispatch_prefix_available(e, row->first) ||
         dispatch_prefix_available(e, row->second);
}

///////////////////////////////////
// LAYOUT

static int box_width(const menu_layout *m, int columns) {
  return 2 * MARGIN + columns * m->column_width + (columns - 1) * GUTTER;
}

// Sections stack with a blank line between them.
static int column_height(const menu_layout *m, int from, int to) {
  int height = 0;
  for (int s = from; s < to; s++)
    height += (s > from) + 1 + m->sections[s].count;
  return height;
}

// Fill `columns` columns with the sections from `from` on, in order, keeping
// the tallest column as short as possible. Writes where each column starts to
// first[0..columns], first[columns] being the end. A handful of sections makes
// trying every split cheap.
static int balance(const menu_layout *m, int from, int columns, int *first) {
  first[0] = from;
  if (columns == 1) {
    first[1] = m->nsections;
    return column_height(m, from, m->nsections);
  }
  int best = -1, rest[MAX_COLUMNS + 1];
  for (int to = from + 1; to <= m->nsections - (columns - 1); to++) {
    int tallest = balance(m, to, columns - 1, rest);
    int here = column_height(m, from, to);
    if (here > tallest)
      tallest = here;
    if (best < 0 || tallest < best) {
      best = tallest;
      memcpy(first + 1, rest, (size_t)columns * sizeof *rest);
    }
  }
  return best;
}

int command_menu_capacity(const editor *e) {
  int rows = e->rows - 6;
  return rows > 0 ? rows : 1;
}

// Size the columns with or without shortcuts, use as many as the terminal is
// wide enough for, and spread the sections over them.
static void arrange(const editor *e, menu_layout *m, int shortcuts) {
  m->shortcuts = shortcuts && m->shortcut_width;
  // A space on each side of the text, as the heading badges have.
  m->column_width = 1 + m->key_width + GAP + m->label_width +
                    (m->shortcuts ? GAP + m->shortcut_width : 0) + 1;
  for (int s = 0; s < m->nsections; s++)
    if ((int)strlen(m->sections[s].title) + 2 > m->column_width)
      m->column_width = (int)strlen(m->sections[s].title) + 2;

  m->columns = 1;
  for (int c = 2; c <= MAX_COLUMNS && c <= m->nsections; c++)
    if (box_width(m, c) <= e->cols)
      m->columns = c;
  if (m->nsections) {
    m->lines = balance(m, 0, m->columns, m->first);
  } else {
    m->first[0] = m->first[1] = 0;
    m->lines = 0;
  }
}

static void layout(const editor *e, menu_layout *m) {
  collect(m);

  m->key_width = m->label_width = m->shortcut_width = 0;
  for (int i = 0; i < m->nrows; i++) {
    char text[600];
    row_keys(&m->rows[i], text, sizeof text);
    if ((int)strlen(text) > m->key_width)
      m->key_width = (int)strlen(text);
    if ((int)strlen(row_label(&m->rows[i])) > m->label_width)
      m->label_width = (int)strlen(row_label(&m->rows[i]));
    row_shortcut(&m->rows[i], text, sizeof text);
    if ((int)strlen(text) > m->shortcut_width)
      m->shortcut_width = (int)strlen(text);
  }
  // Shortcuts are the first thing to go when the menu has to scroll and
  // dropping them leaves less to scroll: it is easier to scan whole.
  arrange(e, m, 0);
  int plain_lines = m->lines;
  arrange(e, m, 1);
  if (m->lines > command_menu_capacity(e) && plain_lines < m->lines)
    arrange(e, m, 0);
}

int command_menu_count(const editor *e) {
  static menu_layout m;
  layout(e, &m);
  return m.lines;
}

void command_menu_scroll(editor *e, int step) {
  int limit = command_menu_count(e) - command_menu_capacity(e);
  if (limit < 0)
    limit = 0;
  e->prefix_scroll += step;
  if (e->prefix_scroll < 0)
    e->prefix_scroll = 0;
  if (e->prefix_scroll > limit)
    e->prefix_scroll = limit;
}

///////////////////////////////////
// DRAWING
//
// A pen writes one row of the box and never past its right edge, so a box
// wider than the terminal is cut off rather than wrapped.

typedef struct {
  abuf *ab;
  int left; // columns still free on this row
} pen;

static void put(pen *p, const char *text) {
  p->left -= screen_text(p->ab, text, p->left);
}

static void pad(pen *p, int count) {
  if (count > p->left)
    count = p->left;
  if (count > 0) {
    screen_repeat(p->ab, ' ', count);
    p->left -= count;
  }
}

static void style(pen *p, const char *sequence) {
  ab_append(p->ab, sequence, (int)strlen(sequence));
}

static void put_key(pen *p, const editor *e, const command_binding *entry) {
  style(p, dispatch_prefix_available(e, entry) ? BOLD : DIM);
  put(p, entry->keys);
  style(p, PLAIN);
}

static void draw_row(pen *p, const editor *e, const menu_layout *m,
                     const menu_row *row) {
  int start = p->left;
  int available = row_available(e, row);
  char keys[600], shortcut[600];
  row_keys(row, keys, sizeof keys);
  row_shortcut(row, shortcut, sizeof shortcut);

  pad(p, 1);
  put_key(p, e, row->first);
  if (row->second) {
    style(p, available ? "" : DIM);
    put(p, "/");
    style(p, PLAIN);
    put_key(p, e, row->second);
  }
  pad(p, m->key_width - (int)strlen(keys) + GAP);

  style(p, available ? "" : DIM);
  put(p, row_label(row));
  style(p, PLAIN);
  if (m->shortcuts) {
    pad(p, m->label_width - (int)strlen(row_label(row)) + GAP +
               m->shortcut_width - (int)strlen(shortcut));
    style(p, DIM);
    put(p, shortcut);
    style(p, PLAIN);
  }
  pad(p, m->column_width - (start - p->left));
}

static void draw_heading(pen *p, const menu_layout *m, const char *title) {
  int start = p->left;
  theme_append(p->ab, THEME_ACTIVE);
  pad(p, 1);
  put(p, title);
  pad(p, 1);
  theme_append(p->ab, THEME_POPUP);
  pad(p, m->column_width - (start - p->left));
}

// Line `line` of column `column`: a heading, a row, or the blank line that
// separates two sections.
static void draw_cell(pen *p, const editor *e, const menu_layout *m,
                      int column, int line) {
  for (int s = m->first[column]; s < m->first[column + 1]; s++) {
    if (s > m->first[column] && line-- == 0)
      break; // the blank line above this section
    if (line == 0) {
      draw_heading(p, m, m->sections[s].title);
      return;
    }
    if (line <= m->sections[s].count) {
      draw_row(p, e, m, &m->rows[m->sections[s].start + line - 1]);
      return;
    }
    line -= 1 + m->sections[s].count;
  }
  pad(p, m->column_width);
}

static void draw_line(pen *p, const editor *e, const menu_layout *m, int line) {
  theme_append(p->ab, THEME_POPUP);
  pad(p, MARGIN);
  for (int c = 0; c < m->columns; c++) {
    if (c)
      pad(p, GUTTER);
    draw_cell(p, e, m, c, line);
  }
  pad(p, p->left);
}

static void draw_bar(pen *p, const char *text) {
  theme_append(p->ab, THEME_ACTIVE);
  int margin = (p->left - (int)strlen(text)) / 2;
  pad(p, margin);
  put(p, text);
  pad(p, p->left);
}

static void footer_text(const menu_layout *m, const editor *e, char *text,
                        size_t size) {
  size_t used = 0;
  text[0] = '\0';
  for (int i = 0; footer_keys[i] != END; i++) {
    const command_binding *entry = dispatch_prefix_lookup(footer_keys[i]);
    if (!entry || used >= size)
      continue;
    // Hints read like the other bottom bars: "?: help", not "?: Help".
    char label[64];
    snprintf(label, sizeof label, "%s", entry->label);
    label[0] = (char)tolower((unsigned char)label[0]);
    used += (size_t)snprintf(text + used, size - used, "%s: %s   ",
                             entry->keys, label);
  }
  if (used < size)
    snprintf(text + used, size - used, "Esc: close%s",
             m->lines > command_menu_capacity(e) ? "   Up/Down: scroll" : "");
}

void command_menu_draw(editor *e, abuf *ab) {
  static menu_layout m;
  int text_rows = e->rows - 2;
  if (text_rows < 1 || e->cols < 1)
    return;
  layout(e, &m);
  command_menu_scroll(e, 0);

  int visible = m.lines < command_menu_capacity(e) ? m.lines
                                                   : command_menu_capacity(e);
  int width = box_width(&m, m.columns);
  if (width < (int)strlen(TITLE) + 4)
    width = (int)strlen(TITLE) + 4;
  if (width > e->cols)
    width = e->cols;
  int height = visible + 4; // title, blank, lines, blank, footer
  if (height > text_rows)
    height = text_rows;
  int left = (e->cols - width) / 2, top = (text_rows - height) / 2 + 1;

  char footer[256];
  footer_text(&m, e, footer, sizeof footer);
  for (int y = 0; y < height; y++) {
    screen_position(ab, left, top + y);
    pen p = {ab, width};
    if (y == 0)
      draw_bar(&p, TITLE);
    else if (y == height - 1)
      draw_bar(&p, footer);
    else if (y == 1 || y == height - 2) {
      theme_append(ab, THEME_POPUP);
      pad(&p, width);
    } else
      draw_line(&p, e, &m, e->prefix_scroll + y - 2);
    theme_append(ab, THEME_NORMAL);
  }
}
