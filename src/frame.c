#include "frame.h"
#include "utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Unchanged cells between two changes on a row are rewritten when that is
// shorter than moving the cursor over them.
#define MERGE_GAP 6
// Longest grapheme cluster read back from a frame.
#define CLUSTER_LIMIT 64
#define SGR_LIMIT 32

// A colour: 0 is the default, 1 to 16 the basic SGR colours (30-37, 90-97),
// INDEXED | n one of 256 indexed colours and DIRECT | rgb a direct colour.
#define INDEXED 0x100
#define DIRECT 0x1000000

// SGR attributes 1 to 9 (6, rapid blink, is not modelled), one bit each.
static const int attribute_codes[] = {1, 2, 3, 4, 5, 7, 8, 9};
#define ATTRIBUTES ((int)(sizeof attribute_codes / sizeof *attribute_codes))

typedef struct style {
  int fg, bg;
  unsigned attributes;
} style;

typedef struct cell {
  int text;             // offset of the cluster in its frame, -1 for a space
  unsigned char bytes;  // length of the cluster
  unsigned char width;  // 1 or 2, and 0 for the right half of a wide cluster
  style look;
} cell;

typedef struct grid {
  cell *cells;
  int rows, cols, valid;
  int cursor_row, cursor_col, cursor_visible;
} grid;

// Like the frame cache in screen.c, both belong to the single terminal UI:
// one grid matches the terminal and the other receives the next frame.
static grid grids[2];
static int shown_grid;

static int attribute_bit(int code) {
  for (int i = 0; i < ATTRIBUTES; i++)
    if (attribute_codes[i] == code)
      return 1 << i;
  return 0;
}

static int same_style(style a, style b) {
  return a.fg == b.fg && a.bg == b.bg && a.attributes == b.attributes;
}

// The colour selected by 38 or 48 and the parameters after it at p[*i].
static int extended_colour(const int *p, int n, int *i, int *colour) {
  if (*i + 2 < n && p[*i + 1] == 5 && p[*i + 2] <= 255) {
    *colour = INDEXED | p[*i + 2];
    *i += 2;
    return 0;
  }
  if (*i + 4 < n && p[*i + 1] == 2 && p[*i + 2] <= 255 && p[*i + 3] <= 255 &&
      p[*i + 4] <= 255) {
    *colour = DIRECT | p[*i + 2] << 16 | p[*i + 3] << 8 | p[*i + 4];
    *i += 4;
    return 0;
  }
  return -1;
}

// Apply one SGR sequence. Returns -1 for a parameter the cache does not model.
static int apply_sgr(style *look, const int *p, int n) {
  if (!n)
    *look = (style){0};
  for (int i = 0; i < n; i++) {
    int v = p[i], bit;
    if (v == 0)
      *look = (style){0};
    else if ((bit = attribute_bit(v)))
      look->attributes |= bit;
    else if (v == 22)
      look->attributes &= ~(unsigned)(attribute_bit(1) | attribute_bit(2));
    else if (v > 22 && v < 30 && (bit = attribute_bit(v - 20)))
      look->attributes &= ~(unsigned)bit;
    else if (v >= 30 && v <= 37)
      look->fg = 1 + v - 30;
    else if (v >= 90 && v <= 97)
      look->fg = 9 + v - 90;
    else if (v == 39)
      look->fg = 0;
    else if (v >= 40 && v <= 47)
      look->bg = 1 + v - 40;
    else if (v >= 100 && v <= 107)
      look->bg = 9 + v - 100;
    else if (v == 49)
      look->bg = 0;
    else if (v == 38 || v == 48) {
      if (extended_colour(p, n, &i, v == 38 ? &look->fg : &look->bg) < 0)
        return -1;
    } else
      return -1;
  }
  return 0;
}

// Measure the cluster at b[i] the way the drawing code measured it.
static int cluster(const char *b, int i, int len, int *bytes, int *width) {
  if ((unsigned char)b[i] < 0x80 &&
      (i + 1 >= len || (unsigned char)b[i + 1] < 0x80)) {
    *bytes = *width = 1;
    return 0;
  }
  char text[CLUSTER_LIMIT + 1];
  int n = 0;
  while (n < CLUSTER_LIMIT && i + n < len) {
    unsigned char c = (unsigned char)b[i + n];
    if (c < 0x20 || c == 0x7f)
      break;
    text[n++] = (char)c;
  }
  text[n] = '\0';
  int next = grapheme_next(text, 0), cp;
  utf8_decode(text, &cp);
  // A leading mark joins the cell before it on the terminal.
  if (next <= 0 || next >= CLUSTER_LIMIT || utf8_width(cp) == 0)
    return -1;
  *bytes = next;
  *width = grapheme_width(text, 0);
  return 0;
}

static void space(cell *c) {
  c->text = -1;
  c->bytes = 1;
  c->width = 1;
}

// Write a cluster as a terminal does: covering half of a wide cluster erases
// the whole of it.
static void put(grid *g, int row, int col, cell value) {
  cell *line = g->cells + (size_t)row * (size_t)g->cols;
  if (line[col].width == 0 && col > 0)
    space(&line[col - 1]);
  if (line[col].width == 2 && col + 1 < g->cols)
    space(&line[col + 1]);
  if (value.width == 2 && line[col + 1].width == 2 && col + 2 < g->cols)
    space(&line[col + 2]);
  line[col] = value;
  if (value.width == 2)
    line[col + 1] = (cell){.text = -1, .width = 0, .look = value.look};
}

// Parameters of the control sequence at b[*i], just after "\x1b[". Returns
// the final byte, or 0 for a sequence the cache does not model.
static char sequence(const char *b, int len, int *i, int *private, int *p,
                     int *n) {
  int at = *i, value = -1;
  *private = at < len && b[at] == '?';
  at += *private;
  *n = 0;
  for (; at < len; at++) {
    char c = b[at];
    if (c >= '0' && c <= '9') {
      value = (value < 0 ? 0 : value) * 10 + c - '0';
      if (value > 65535)
        return 0;
    } else if (c == ';') {
      if (*n == SGR_LIMIT)
        return 0;
      p[(*n)++] = value < 0 ? 0 : value;
      value = -1;
    } else
      break;
  }
  if (at >= len || b[at] < '@' || b[at] > '~')
    return 0;
  if (value >= 0 || *n) {
    if (*n == SGR_LIMIT)
      return 0;
    p[(*n)++] = value < 0 ? 0 : value;
  }
  *i = at + 1;
  return b[at];
}

// Read a frame back into cells. Returns -1 when it is not plain positioned
// text, SGR colours and cursor visibility.
static int parse(const char *b, int len, grid *g) {
  int rows = g->rows, cols = g->cols;
  cell blank = {0};
  space(&blank);
  for (size_t k = 0; k < (size_t)rows * (size_t)cols; k++)
    g->cells[k] = blank;
  style look = {0};
  int row = 0, col = 0;
  g->cursor_visible = 1;
  for (int i = 0; i < len; ) {
    unsigned char c = (unsigned char)b[i];
    if (c == 0x1b) {
      int p[SGR_LIMIT], n, private;
      if (i + 1 >= len || b[i + 1] != '[')
        return -1;
      i += 2;
      char final = sequence(b, len, &i, &private, p, &n);
      if (private) {
        if ((final != 'h' && final != 'l') || n != 1)
          return -1;
        if (p[0] == 25)
          g->cursor_visible = final == 'h';
        else if (p[0] != 2026)
          return -1;
      } else if (final == 'H' || final == 'f') {
        int r = n > 0 && p[0] ? p[0] : 1, k = n > 1 && p[1] ? p[1] : 1;
        row = (r < rows ? r : rows) - 1;
        col = (k < cols ? k : cols) - 1;
      } else if (final != 'm' || apply_sgr(&look, p, n) < 0)
        return -1;
      continue;
    }
    if (c == '\t') { // the terminal moves to its next tab stop
      int stop = (col / 8 + 1) * 8;
      col = stop < cols ? stop : cols - 1;
      i++;
      continue;
    }
    int bytes, width;
    if (c < 0x20 || c == 0x7f || cluster(b, i, len, &bytes, &width) < 0 ||
        col + width > cols)
      return -1;
    cell value = {.text = i, .bytes = (unsigned char)bytes,
                  .width = (unsigned char)width, .look = look};
    if (bytes == 1 && c == ' ')
      value.text = -1;
    put(g, row, col, value);
    col += width;
    i += bytes;
  }
  g->cursor_row = row;
  g->cursor_col = col < cols ? col : cols - 1;
  return 0;
}

static int same_cell(const cell *a, const char *a_text, const cell *b,
                     const char *b_text) {
  if (a->width != b->width || a->bytes != b->bytes ||
      (a->text < 0) != (b->text < 0) || !same_style(a->look, b->look))
    return 0;
  return a->text < 0 || !memcmp(a_text + a->text, b_text + b->text, a->bytes);
}

static void put_position(abuf *out, int row, int col) {
  char text[32];
  int n = snprintf(text, sizeof text, "\x1b[%d;%dH", row + 1, col + 1);
  ab_append(out, text, n);
}

static int put_colour(char *text, size_t size, int colour, int base) {
  if (!colour)
    return 0;
  if (colour <= 8)
    return snprintf(text, size, ";%d", base + colour - 1);
  if (colour <= 16)
    return snprintf(text, size, ";%d", base + 60 + colour - 9);
  if (colour & DIRECT)
    return snprintf(text, size, ";%d;2;%d;%d;%d", base + 8,
                    colour >> 16 & 255, colour >> 8 & 255, colour & 255);
  return snprintf(text, size, ";%d;5;%d", base + 8, colour & 255);
}

static void put_style(abuf *out, style look) {
  char text[96];
  int n = snprintf(text, sizeof text, "\x1b[0");
  for (int i = 0; i < ATTRIBUTES; i++)
    if (look.attributes & 1u << i)
      n += snprintf(text + n, sizeof text - n, ";%d", attribute_codes[i]);
  n += put_colour(text + n, sizeof text - n, look.fg, 30);
  n += put_colour(text + n, sizeof text - n, look.bg, 40);
  text[n++] = 'm';
  ab_append(out, text, n);
}

int frame_update(const abuf *shown, const abuf *next, int rows, int cols,
                 abuf *out) {
  grid *old = &grids[shown_grid], *g = &grids[!shown_grid];
  if (rows <= 0 || cols <= 0 || !next->b ||
      (size_t)rows > ((size_t)-1 / sizeof(cell)) / (size_t)cols)
    return -1;
  if (!g->cells || g->rows != rows || g->cols != cols) {
    cell *cells = realloc(g->cells, (size_t)rows * (size_t)cols * sizeof *cells);
    if (!cells)
      return -1;
    g->cells = cells;
    g->rows = rows;
    g->cols = cols;
  }
  g->valid = 0;
  if (parse(next->b, next->len, g) < 0)
    return -1;

  int full = !old->valid || !shown->b || old->rows != rows || old->cols != cols;
  const char *was = shown->b, *now = next->b;
  int start = out->len, known = 0, at_row = 0, at_col = 0, styled = 0;
  style current = {0};
  // Hide the cursor while cells change; terminals with synchronized output
  // show the update in one step.
  ab_append(out, "\x1b[?2026h\x1b[?25l", 14);
  int header = out->len;
  for (int y = 0; y < rows; y++) {
    const cell *line = g->cells + (size_t)y * (size_t)cols;
    const cell *before = full ? NULL : old->cells + (size_t)y * (size_t)cols;
    for (int x = 0; x < cols; ) {
      if (!full && same_cell(&before[x], was, &line[x], now)) {
        x++;
        continue;
      }
      int first = line[x].width == 0 && x > 0 ? x - 1 : x;
      int last = x;
      for (int j = x + 1, gap = 0; j < cols && gap <= MERGE_GAP; j++) {
        if (full || !same_cell(&before[j], was, &line[j], now)) {
          last = j;
          gap = 0;
        } else
          gap++;
      }
      for (int j = first; j <= last; j++) {
        const cell *c = &line[j];
        if (c->width == 0)
          continue; // drawn with the left half
        if (!known || at_row != y || at_col != j)
          put_position(out, y, j);
        if (!styled || !same_style(current, c->look)) {
          put_style(out, c->look);
          current = c->look;
          styled = 1;
        }
        if (c->text < 0)
          ab_append(out, " ", 1);
        else
          ab_append(out, now + c->text, c->bytes);
        at_row = y;
        at_col = j + c->width;
        // The terminal may measure a non-ASCII cluster differently, and the
        // last column leaves a pending wrap: position the next cell anew.
        known = c->bytes == 1 && at_col < cols;
      }
      x = last + (line[last].width == 2 ? 2 : 1);
    }
  }
  int cursor_moved = full || old->cursor_visible != g->cursor_visible ||
      (g->cursor_visible && (old->cursor_row != g->cursor_row ||
                             old->cursor_col != g->cursor_col));
  if (out->len == header && !cursor_moved) {
    out->len = start;
    return 0;
  }
  if (g->cursor_visible) {
    put_position(out, g->cursor_row, g->cursor_col);
    ab_append(out, "\x1b[?25h", 6);
  }
  ab_append(out, "\x1b[?2026l", 8);
  return 1;
}

void frame_shown(void) {
  shown_grid = !shown_grid;
  grids[shown_grid].valid = 1;
}

void frame_forget(void) {
  for (int i = 0; i < 2; i++) {
    free(grids[i].cells);
    grids[i] = (grid){0};
  }
}
