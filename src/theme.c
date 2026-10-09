#include "theme.h"
#include "screen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Like the command registry, the palette belongs to the single terminal UI.
static theme_palette current;
static int configured;
static const char *names[THEME_COUNT] = {
    "normal",     "status",       "warning",      "selection", "active",
    "inactive",   "muted",        "accent",       "logo",      "popup",
    "diff_added", "diff_removed", "diff_context", "diff_hunk", "diff_meta",
    "keyword",    "type",         "string",       "comment",   "number",
    "directive",  "function",
};
static const char *defaults[THEME_COUNT] = {
    "\x1b[m",
    "\x1b[30;103m",
    "\x1b[97;41m",
#ifdef _WIN32
    "\x1b[0;30;43m",
    "\x1b[0;30;43m",
#else
    "\x1b[44;97m",
    "\x1b[97;44m",
#endif
    "\x1b[30;47m",
    "\x1b[90m",
    "\x1b[36m",
    "\x1b[1;93m",
    "\x1b[37;48;5;235m",
    "\x1b[0;38;5;194;48;5;22m",
    "\x1b[0;38;5;224;48;5;52m",
    "\x1b[0;38;5;252;48;5;235m",
    "\x1b[0;36m",
    "\x1b[0;90m",
    "\x1b[95m",
    "\x1b[96m",
    "\x1b[92m",
    "\x1b[90m",
    "\x1b[93m",
    "\x1b[94m",
    "\x1b[36m",
};
void theme_defaults(theme_palette *palette) {
  memset(palette, 0, sizeof *palette);
  for (int i = 0; i < THEME_COUNT; i++)
    snprintf(palette->colours[i], sizeof palette->colours[i], "%s",
             defaults[i]);
}
void theme_use(const theme_palette *palette) {
  configured = palette != NULL;
  if (palette)
    current = *palette;
}
const char *theme_colour(theme_role role) {
  if (role < 0 || role >= THEME_COUNT)
    role = THEME_NORMAL;
  return configured ? current.colours[role] : defaults[role];
}
const char *theme_role_name(theme_role role) {
  return role >= 0 && role < THEME_COUNT ? names[role] : NULL;
}
void theme_append(struct abuf *ab, theme_role role) {
  const char *colour = theme_colour(role);
  ab_append(ab, colour, (int)strlen(colour));
}
static int component(const char *text, int foreground, char *out, size_t size) {
  if (!strcmp(text, "default")) {
    snprintf(out, size, "%d", foreground ? 39 : 49);
    return 0;
  }
  if (!*text)
    return -1;
  for (const char *p = text; *p; p++)
    if (*p < '0' || *p > '9')
      return -1;
  if (strlen(text) > 3)
    return -1;
  int value = (int)strtol(text, NULL, 10);
  if (value < 0 || value > 255)
    return -1;
  snprintf(out, size, "%d;5;%d", foreground ? 38 : 48, value);
  return 0;
}
int theme_set(theme_palette *palette, const char *name, const char *value) {
  int role = 0;
  while (role < THEME_COUNT && strcmp(name, names[role]))
    role++;
  if (role == THEME_COUNT || strlen(value) >= 64)
    return -1;
  char text[64], fg[24], bg[24];
  strcpy(text, value);
  char *comma = strchr(text, ',');
  if (!comma)
    return -1;
  *comma++ = 0;
  // Spaces around the comma are accepted, never terminal escape sequences.
  char *end = text + strlen(text);
  while (end > text && end[-1] == ' ')
    *--end = 0;
  while (*comma == ' ')
    comma++;
  if (component(text, 1, fg, sizeof fg) || component(comma, 0, bg, sizeof bg))
    return -1;
  snprintf(palette->colours[role], sizeof palette->colours[role],
           "\x1b[0;%s;%sm", fg, bg);
  return role;
}
