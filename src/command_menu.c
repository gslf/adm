#include "theme.h"
#include "command_menu.h"
#include "screen.h"

#include <stdio.h>
#include <string.h>

static const command_binding *partner(const editor *e, const command_binding *entry) {
  if (!entry->pair_label)
    return NULL;
  const command_binding *other = dispatch_prefix_find(e, entry->pair_key);
  return other && other->pair_key == entry->key &&
         other->pair_primary == entry->pair_primary && other->pair_label &&
         (other->key < 32) == (entry->key < 32) ? other : NULL;
}

static int secondary(const editor *e, const command_binding *entry) {
  return partner(e, entry) && entry->key != entry->pair_primary;
}

static int section_count(const editor *e, int control) {
  int rows = 0, count = dispatch_prefix_count(e);
  for (int i = 0; i < count; i++) {
    const command_binding *entry = dispatch_prefix_at(e, i);
    rows += (entry->key < 32) == control && !secondary(e, entry);
  }
  return rows;
}

int command_menu_count(const editor *e) {
  int plain = section_count(e, 0), control = section_count(e, 1);
  return plain + control + (plain > 0 && control > 0);
}

// Plain suffixes precede Ctrl suffixes, with a blank row between the groups.
static const command_binding *menu_entry(const editor *e, int row) {
  int plain = section_count(e, 0), control = section_count(e, 1);
  int section = row >= plain;
  if (section) {
    row -= plain;
    if (plain > 0 && control > 0 && row-- == 0)
      return NULL;
  }
  int count = dispatch_prefix_count(e);
  for (int i = 0; i < count; i++) {
    const command_binding *entry = dispatch_prefix_at(e, i);
    if ((entry->key < 32) == section && !secondary(e, entry) && row-- == 0)
      return entry;
  }
  return NULL;
}

static void binding_keys(const command_binding *entry, char *keys, size_t size) {
  char direct[256];
  dispatch_direct_bindings(entry->cmd, direct, sizeof direct);
  if (direct[0])
    snprintf(keys, size, "%s [%s]", entry->keys, direct);
  else
    snprintf(keys, size, "%s", entry->keys);
}

static void row_text(const editor *e, int row, char *keys, size_t size,
                     const char **label) {
  const command_binding *entry = menu_entry(e, row);
  if (!entry) {
    keys[0] = '\0';
    *label = "";
    return;
  }
  const command_binding *other = partner(e, entry);
  *label = other ? entry->pair_label : entry->label;
  if (other) {
    char first[320], second[320];
    binding_keys(entry, first, sizeof first);
    binding_keys(other, second, sizeof second);
    snprintf(keys, size, "%s / %s", first, second);
  } else
    binding_keys(entry, keys, size);
}

int command_menu_capacity(const editor *e) {
  int rows = e->rows - 6;
  return rows > 0 ? rows : 1;
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

static void centered(char *line, size_t size, const char *text, int width) {
  int margin = (width - (int)strlen(text)) / 2;
  snprintf(line, size, "%*s%s", margin > 0 ? margin : 0, "", text);
}

// Compute columns from the entire visible menu so scrolling keeps rows aligned.
void command_menu_draw(editor *e, abuf *ab) {
  int count = command_menu_count(e);
  int text_rows = e->rows - 2;
  if (text_rows < 1 || e->cols < 1)
    return;
  command_menu_scroll(e, 0);
  int key_width = 0, label_width = 0;
  for (int i = 0; i < count; i++) {
    char keys[672];
    const char *label;
    row_text(e, i, keys, sizeof keys, &label);
    int length = (int)strlen(keys);
    if (length > key_width)
      key_width = length;
    length = (int)strlen(label);
    if (length > label_width)
      label_width = length;
  }
  int width = key_width + label_width + 7;
  if (width < 52)
    width = 52;
  if (width > e->cols)
    width = e->cols;
  int height = count + 4;
  if (height > text_rows)
    height = text_rows;
  int left = (e->cols - width) / 2 + 1;
  int top = (text_rows - height) / 2 + 2;

  for (int y = 0; y < height; y++) {
    char pos[40], line[1024] = {0};
    int n = snprintf(pos, sizeof pos, "\x1b[%d;%dH", top + y, left);
    ab_append(ab, pos, n);
    theme_append(ab, THEME_ACTIVE);
    int border = y == 0 || y == height - 1;
    if (y == 1)
      centered(line, sizeof line, "][ adm - Command Center", width - 2);
    else if (y >= 2 && y < height - 2) {
      char keys[672];
      const char *label;
      row_text(e, e->prefix_scroll + y - 2, keys, sizeof keys, &label);
      if (keys[0])
        snprintf(line, sizeof line, " %-*s - %s", key_width, keys, label);
    } else if (y == height - 2) {
      centered(line, sizeof line, count > command_menu_capacity(e)
          ? "Up/Down scroll; C-g / Esc cancel" : "C-g / Esc cancel", width - 2);
    }
    int len = (int)strlen(line);
    for (int x = 0; x < width; x++) {
      char c = border ? (x == 0 || x == width - 1 ? '+' : '-')
                     : x == 0 || x == width - 1 ? '|'
                     : x - 1 < len ? line[x - 1] : ' ';
      ab_append(ab, &c, 1);
    }
    theme_append(ab, THEME_NORMAL);
  }
}
