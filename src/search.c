// Incremental search with a small regular expression engine, and the
// go-to-line prompt that shares its bar.
//
// Ctrl-F opens a prompt on the bottom status bar. The search is incremental:
// each keystroke runs it again from where the cursor was, and the first match
// is shown highlighted. Enter closes the prompt and leaves the cursor on the
// match, Esc closes it and puts everything back, and the arrow keys walk the
// matches: Down (or Ctrl-F again) to the next one, Up to the previous one,
// wrapping around the ends of the file.
//
// Ctrl-G opens the same prompt as GOTO: type a line number and the view
// follows it as it is typed, Enter stays there, Esc goes back. A number past
// the end of the file stops at the last line.
//
// The pattern language is the classic core of regular expressions:
//
//   c          a literal character (UTF-8 aware)
//   .          any single character
//   ^  $       start and end of the line, special only in those positions
//   [...]      a character class, with ranges like a-z and negation [^...]
//   \d \w \s   digit, word character, whitespace; \D \W \S their opposites
//   \c         the literal character c, for when c is special
//   * + ?      zero or more, one or more, zero or one of the last element
//
// The engine is deliberately the smallest thing that honestly earns the name
// regex: no groups, no alternation, no back references. Patterns are
// interpreted straight from their own text, one line at a time, with greedy
// backtracking on the quantifiers. Nothing beyond the C standard library,
// which is what lets the same code run everywhere the editor does.

#include "search.h"
#include "cursor.h"
#include "screen.h"
#include "utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

///////////////////////////////////
// REGEX ENGINE

static int is_digit_cp(int cp) {
  return cp >= '0' && cp <= '9';
}

static int is_space_cp(int cp) {
  return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' || cp == '\f' ||
         cp == '\v';
}

// The same idea of a word the cursor uses for Ctrl-Left and Ctrl-Right:
// letters, digits, underscore, and everything outside ASCII.
static int is_word_cp(int cp) {
  return is_digit_cp(cp) || (cp >= 'a' && cp <= 'z') ||
         (cp >= 'A' && cp <= 'Z') || cp == '_' || cp >= 0x80;
}

// Does the class escape letter c accept the code point? Returns -1 when c is
// no class letter at all, so the caller falls back to a literal match.
static int class_escape(char c, int cp) {
  switch (c) {
    case 'd': return is_digit_cp(cp);
    case 'D': return !is_digit_cp(cp);
    case 'w': return is_word_cp(cp);
    case 'W': return !is_word_cp(cp);
    case 's': return is_space_cp(cp);
    case 'S': return !is_space_cp(cp);
    default:  return -1;
  }
}

// Bytes taken by the single pattern element starting at p, not counting any
// quantifier behind it. Returns 0 when the element is malformed: a backslash
// with nothing after it, or a class that never closes.
static int elem_len(const char *p) {
  int cp;

  if (*p == '\\') {
    if (p[1] == '\0')
      return 0;
    return 1 + utf8_decode(p + 1, &cp);
  }

  if (*p == '[') {
    int i = 1;
    if (p[i] == '^')
      i++;
    if (p[i] == ']')
      i++; // a ']' first thing in the class is a literal, not the end
    while (p[i] != ']') {
      if (p[i] == '\0')
        return 0;
      if (p[i] == '\\') {
        if (p[i + 1] == '\0')
          return 0;
        i++;
      }
      i += utf8_decode(p + i, &cp);
    }
    return i + 1;
  }

  return utf8_decode(p, &cp);
}

// Does the class starting at p, which points at its '[', accept cp?
static int class_match(const char *p, int cp) {
  int i = 1;
  int negate = 0;
  if (p[i] == '^') {
    negate = 1;
    i++;
  }

  int hit = 0;
  int first = 1; // lets a leading ']' through as a literal
  while (p[i] != ']' || first) {
    first = 0;
    int lo;

    if (p[i] == '\\') {
      int r = class_escape(p[i + 1], cp);
      if (r >= 0) {
        if (r)
          hit = 1;
        i += 2;
        continue;
      }
      i++;
      i += utf8_decode(p + i, &lo);
    } else {
      i += utf8_decode(p + i, &lo);
    }

    // A range like a-z, unless the '-' is the last thing before the ']'.
    if (p[i] == '-' && p[i + 1] != ']' && p[i + 1] != '\0') {
      int hi;
      i++;
      if (p[i] == '\\')
        i++;
      i += utf8_decode(p + i, &hi);
      if (cp >= lo && cp <= hi)
        hit = 1;
    } else if (cp == lo) {
      hit = 1;
    }
  }

  return negate ? !hit : hit;
}

// Match the one element at p against the text at t. On success returns 1 and
// stores in *tlen the bytes of text it covered; an element never matches
// empty text, so that is always at least one.
static int elem_match(const char *p, const char *t, int *tlen) {
  if (*t == '\0')
    return 0;

  int cp;
  int n = utf8_decode(t, &cp);

  int ok;
  int lit;
  if (*p == '.') {
    ok = 1;
  } else if (*p == '[') {
    ok = class_match(p, cp);
  } else if (*p == '\\') {
    int r = class_escape(p[1], cp);
    if (r >= 0) {
      ok = r;
    } else {
      utf8_decode(p + 1, &lit);
      ok = (cp == lit);
    }
  } else {
    utf8_decode(p, &lit);
    ok = (cp == lit);
  }

  if (!ok)
    return 0;
  *tlen = n;
  return 1;
}

static int match_here(const char *p, const char *t);

// A quantified element: take as many repetitions as the text gives, then
// hand them back one at a time until the rest of the pattern fits. min is 1
// for '+', max is 1 for '?' and -1 when there is no limit.
static int match_quant(const char *p, int el, int min, int max,
                       const char *t) {
  const char *rest = p + el + 1;

  int count = 0, total = 0, tlen;
  while ((max < 0 || count < max) && elem_match(p, t + total, &tlen)) {
    total += tlen;
    count++;
  }

  while (count >= min) {
    int r = match_here(rest, t + total);
    if (r >= 0)
      return total + r;
    if (count == 0)
      break;

    // One repetition fewer. Repetitions vary in byte length, so the offset
    // of the shorter run is recounted from the start rather than stored.
    count--;
    total = 0;
    for (int i = 0; i < count; i++) {
      elem_match(p, t + total, &tlen);
      total += tlen;
    }
  }

  return -1;
}

// Match the pattern p against the text at t. Returns the bytes of text the
// whole pattern covered, or -1 if it does not match here. '$' is the end
// anchor only as the last character of the pattern, a literal anywhere else;
// '^' is dealt with by the caller before this is ever reached.
static int match_here(const char *p, const char *t) {
  if (*p == '\0')
    return 0;
  if (*p == '$' && p[1] == '\0')
    return (*t == '\0') ? 0 : -1;

  int el = elem_len(p);
  if (el == 0)
    return -1; // malformed, which validation keeps from ever getting here

  char q = p[el];
  if (q == '*')
    return match_quant(p, el, 0, -1, t);
  if (q == '+')
    return match_quant(p, el, 1, -1, t);
  if (q == '?')
    return match_quant(p, el, 0, 1, t);

  int tlen;
  if (!elem_match(p, t, &tlen))
    return -1;

  int r = match_here(p + el, t + tlen);
  return (r < 0) ? -1 : tlen + r;
}

// Is the pattern well formed? Checked once, when it is typed, so the matcher
// itself can take a sound pattern for granted.
static int pattern_ok(const char *p) {
  if (*p == '^')
    p++;

  int have_elem = 0; // is there an element for a quantifier to repeat?
  while (*p) {
    if (*p == '*' || *p == '+' || *p == '?') {
      if (!have_elem)
        return 0;
      have_elem = 0; // without groups, "a**" repeats nothing
      p++;
      continue;
    }
    int el = elem_len(p);
    if (el == 0)
      return 0;
    p += el;
    have_elem = 1;
  }
  return 1;
}

// Leftmost match on the line at or after byte offset from. Returns the byte
// offset of the match and stores its byte length in *len, or returns -1.
static int line_find(const char *pat, const char *line, int from, int *len) {
  int anchored = (pat[0] == '^');
  const char *p = anchored ? pat + 1 : pat;
  int llen = (int)strlen(line);

  if (anchored && from > 0)
    return -1;

  for (int i = from; i <= llen;) {
    int r = match_here(p, line + i);
    if (r >= 0) {
      *len = r;
      return i;
    }
    if (anchored || i == llen)
      break;
    int cp;
    i += utf8_decode(line + i, &cp);
  }
  return -1;
}

// Rightmost match that starts strictly before byte offset before; pass a
// value past the line end to take the whole line. Returns as line_find does.
static int line_rfind(const char *pat, const char *line, int before,
                      int *len) {
  int best = -1, blen = 0;

  for (int from = 0;;) {
    int mlen;
    int x = line_find(pat, line, from, &mlen);
    if (x < 0 || x >= before)
      break;
    best = x;
    blen = mlen;

    // On to the next start position. One whole character forward, not one
    // match forward, so matches that overlap are not stepped over.
    if (line[x] == '\0')
      break;
    int cp;
    from = x + utf8_decode(line + x, &cp);
  }

  if (best >= 0)
    *len = blen;
  return best;
}

///////////////////////////////////
// SEARCHING THE BUFFER

// The first match at or after (from_y, from_x), wrapping past the end of the
// buffer and giving up after coming full circle.
static int find_forward(editor *e, const char *pat, int from_y, int from_x,
                        int *my, int *mx, int *mlen) {
  int n = e->buf.nlines;
  if (n <= 0)
    return 0;

  for (int i = 0; i <= n; i++) {
    int y = (from_y + i) % n;
    const char *line = buffer_line(&e->buf, y);
    if (!line)
      continue;

    int len;
    int x = line_find(pat, line, (i == 0) ? from_x : 0, &len);
    if (x >= 0) {
      *my = y;
      *mx = x;
      *mlen = len;
      return 1;
    }
  }
  return 0;
}

// The same thing backwards: the last match before (before_y, before_x).
static int find_backward(editor *e, const char *pat, int before_y,
                         int before_x, int *my, int *mx, int *mlen) {
  int n = e->buf.nlines;
  if (n <= 0)
    return 0;

  for (int i = 0; i <= n; i++) {
    int y = ((before_y - i) % n + n) % n;
    const char *line = buffer_line(&e->buf, y);
    if (!line)
      continue;

    // Only the line the search leaves from is cut short; after the wrap it
    // comes around again whole, one past its end so a match on '$' counts.
    int limit = (i == 0) ? before_x : (int)strlen(line) + 1;
    int len;
    int x = line_rfind(pat, line, limit, &len);
    if (x >= 0) {
      *my = y;
      *mx = x;
      *mlen = len;
      return 1;
    }
  }
  return 0;
}

///////////////////////////////////
// SEARCH STATE

#define QUERY_MAX 256

static struct {
  int active;            // the prompt is open and owns the keyboard
  int goto_mode;         // the prompt is a GOTO one: the query is a line number
  char query[QUERY_MAX]; // the pattern as typed, NUL terminated
  int qlen;
  int valid;             // the pattern is well formed
  int found;             // a match is on show
  int my, mx, mlen;      // the match on show: position and byte length
  int oy, ox;            // where the cursor was when the prompt opened,
  int orowoff, ocoloff;  // and how the screen was scrolled, for Esc
  int osticky;
} S;

// Put the cursor on the match and light the match up. The highlight borrows
// the selection: the anchor sits at the end of the match and the cursor at
// its start, which is also where the scrolling brings into view.
static void show_match(editor *e) {
  e->cy = S.my;
  e->cx = S.mx;
  e->sely = S.my;
  e->selx = S.mx + S.mlen;
  e->sel_active = (S.mlen > 0); // a '^' or '$' match has no width to paint
  e->sel_mode = 0;
  cursor_mark_column(e);
}

// Put the view back the way it was when the prompt opened.
static void restore_view(editor *e) {
  e->cy = S.oy;
  e->cx = S.ox;
  e->rowoff = S.orowoff;
  e->coloff = S.ocoloff;
  e->sticky = S.osticky;
  e->sel_active = 0;
}

// Run the search again from the position the prompt opened at. Called after
// every change to the query, which is what makes the search incremental.
static void search_update(editor *e) {
  S.valid = pattern_ok(S.query);
  S.found = 0;

  if (!S.valid || S.qlen == 0) {
    restore_view(e);
    return;
  }

  S.found = find_forward(e, S.query, S.oy, S.ox, &S.my, &S.mx, &S.mlen);
  if (S.found)
    show_match(e);
  else
    restore_view(e);
}

static void search_next(editor *e) {
  if (!S.found)
    return;

  // Start one character past the start of the current match, not past its
  // end, so matches that overlap it are still found.
  int y = S.my, x = 0;
  const char *line = buffer_line(&e->buf, S.my);
  if (line && line[S.mx] != '\0') {
    int cp;
    x = S.mx + utf8_decode(line + S.mx, &cp);
  } else {
    y = (e->buf.nlines > 0) ? (S.my + 1) % e->buf.nlines : 0;
  }

  if (find_forward(e, S.query, y, x, &S.my, &S.mx, &S.mlen))
    show_match(e);
}

static void search_prev(editor *e) {
  if (!S.found)
    return;

  if (find_backward(e, S.query, S.my, S.mx, &S.my, &S.mx, &S.mlen))
    show_match(e);
}

// The GOTO twin of search_update: the query is a line number, and the view
// follows it as it is typed, clamped to the lines the file actually has.
static void goto_update(editor *e) {
  if (S.qlen == 0) {
    restore_view(e);
    return;
  }

  long last = (e->buf.nlines > 0) ? e->buf.nlines - 1 : 0;
  long y = strtol(S.query, NULL, 10) - 1;
  if (y > last)
    y = last;
  if (y < 0)
    y = 0;

  e->cy = (int)y;
  e->cx = 0;
  cursor_mark_column(e);
}

static void prompt_update(editor *e) {
  if (S.goto_mode)
    goto_update(e);
  else
    search_update(e);
}

// Open the prompt, either kind, remembering where the view was for Esc.
static void prompt_open(editor *e, int goto_mode) {
  S.active = 1;
  S.goto_mode = goto_mode;
  S.qlen = 0;
  S.query[0] = '\0';
  S.valid = 1;
  S.found = 0;

  S.oy = e->cy;
  S.ox = e->cx;
  S.orowoff = e->rowoff;
  S.ocoloff = e->coloff;
  S.osticky = e->sticky;

  // Whatever was selected is let go: from here until the prompt closes the
  // selection machinery is borrowed to show matches.
  selection_clear(e);
}

// Command - open the search prompt
static void search_begin(editor *e) {
  prompt_open(e, 0);
}

// Command - open the go-to-line prompt
static void goto_begin(editor *e) {
  prompt_open(e, 1);
}

static void search_close(editor *e, int accept) {
  S.active = 0;
  e->sel_active = 0;
  e->sel_mode = 0;

  // On Enter the cursor stays on the match, where show_match left it; only
  // Esc walks everything back.
  if (!accept)
    restore_view(e);
}

///////////////////////////////////
// KEYS

static void query_insert(editor *e, int key) {
  char seq[4];
  int n = utf8_encode(key, seq);
  if (n == 0 || S.qlen + n >= QUERY_MAX)
    return; // a query longer than the buffer is quietly not extended

  memcpy(S.query + S.qlen, seq, n);
  S.qlen += n;
  S.query[S.qlen] = '\0';
  prompt_update(e);
}

static void query_backspace(editor *e) {
  if (S.qlen == 0)
    return;

  S.qlen = grapheme_prev(S.query, S.qlen);
  S.query[S.qlen] = '\0';
  prompt_update(e);
}

// The GOTO prompt takes digits and nothing else; Enter with an empty query
// is the same as Esc, there is nowhere it could go.
static int goto_on_key(editor *e, int key) {
  switch (key) {
    case '\x1b':
      search_close(e, 0);
      return 1;

    case '\r':
    case '\n':
      search_close(e, S.qlen > 0);
      return 1;

    case KEY_BACKSPACE:
    case CTRL('h'):
      query_backspace(e);
      return 1;
  }

  // Nine digits at most: enough for any file, few enough for a long.
  if (key >= '0' && key <= '9' && S.qlen < 9)
    query_insert(e, key);

  return 1;
}

static int search_on_key(editor *e, int key) {
  if (!S.active)
    return 0;

  if (S.goto_mode)
    return goto_on_key(e, key);

  switch (key) {
    case '\x1b':
      search_close(e, 0);
      return 1;

    case '\r':
    case '\n':
      search_close(e, 1);
      return 1;

    case KEY_BACKSPACE:
    case CTRL('h'): // what Backspace is on some terminals
      query_backspace(e);
      return 1;

    case KEY_DOWN:
    case CTRL('f'):
      search_next(e);
      return 1;

    case KEY_UP:
      search_prev(e);
      return 1;
  }

  if (key >= 32 && key < KEY_SPECIAL)
    query_insert(e, key);

  // While the prompt is open every key belongs to it, the ones it has no
  // use for included: a stray PgDn must not move the cursor underneath it.
  return 1;
}

///////////////////////////////////
// PROMPT BAR

// The same palette as the rest of the screen: the badge is the selection
// blue so the bar and the highlighted match read as one thing, and the bar
// itself turns to the warning colours when the pattern is broken or finds
// nothing, which is an answer too.
#define BAR_COLOURS     "\x1b[30;103m" // black on bright yellow
#define BAR_BAD_COLOURS "\x1b[97;41m"  // white on red
#define SEARCH_BADGE    "\x1b[97;44;1m SEARCH \x1b[22m"
#define SEARCH_BADGE_COLS 8 // visible width of " SEARCH "
#define GOTO_BADGE      "\x1b[97;44;1m GOTO \x1b[22m"
#define GOTO_BADGE_COLS 6 // visible width of " GOTO "

static void append_str(abuf *ab, const char *s) {
  ab_append(ab, s, (int)strlen(s));
}

// Byte offset of the tail of q that fits in cols screen columns. A query too
// long for the bar keeps its end on show, which is where the typing happens.
// Reports in *used the columns that tail takes.
static int tail_offset(const char *q, int cols, int *used) {
  int total = utf8_cols(q, (int)strlen(q));
  int start = 0;
  while (total > cols && q[start]) {
    total -= grapheme_width(q, start);
    start = grapheme_next(q, start);
  }
  *used = total;
  return start;
}

static void search_on_draw(editor *e, abuf *ab) {
  if (!S.active)
    return;

  // The bottom status bar is already drawn; this paints the prompt over it.
  char tmp[32];
  int n = snprintf(tmp, sizeof tmp, "\x1b[%d;1H", e->rows);
  ab_append(ab, tmp, n);

  append_str(ab, S.goto_mode ? GOTO_BADGE : SEARCH_BADGE);

  // The GOTO prompt is never in trouble: only digits get in, and a number
  // past the end of the file just stops at the last line.
  int trouble = !S.goto_mode && (!S.valid || (S.qlen > 0 && !S.found));
  append_str(ab, trouble ? BAR_BAD_COLOURS : BAR_COLOURS);

  const char *note = S.goto_mode ? ""
                     : !S.valid ? "  bad pattern"
                     : (S.qlen > 0 && !S.found) ? "  no match" : "";

  int avail = e->cols - (S.goto_mode ? GOTO_BADGE_COLS : SEARCH_BADGE_COLS);
  if (avail < 0)
    avail = 0;

  // The note is plain ASCII, so its bytes are its columns. It is clipped
  // before the query is: seeing what was typed matters more.
  int notelen = (int)strlen(note);
  if (notelen > avail - 1)
    notelen = (avail > 1) ? avail - 1 : 0;

  int qcols;
  int qstart = tail_offset(S.query, avail - 1 - notelen, &qcols);

  int used = 0;
  if (avail > 0) {
    ab_append(ab, " ", 1);
    ab_append(ab, S.query + qstart, S.qlen - qstart);
    ab_append(ab, note, notelen);
    used = 1 + qcols + notelen;
  }
  for (int i = used; i < avail; i++)
    ab_append(ab, " ", 1);

  append_str(ab, "\x1b[m");
}

///////////////////////////////////
// MODULE

static void search_init(editor *e) {
  (void)e;
  dispatch_bind(CTRL('f'), search_begin);
  dispatch_bind(CTRL('g'), goto_begin);
}

static module search = {
  .name = "search",
  .init = search_init,
  .on_key = search_on_key,
  .on_draw = search_on_draw,
  .on_change = NULL,
  .shutdown = NULL
};

module *search_module(void) {
  return &search;
}
