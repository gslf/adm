#ifndef ADM_THEME_H
#define ADM_THEME_H
#include <stddef.h>
typedef enum theme_role {
  THEME_NORMAL,
  THEME_STATUS,
  THEME_WARNING,
  THEME_SELECTION,
  THEME_ACTIVE,
  THEME_INACTIVE,
  THEME_MUTED,
  THEME_ACCENT,
  THEME_LOGO,
  THEME_POPUP,
  THEME_DIFF_ADDED,
  THEME_DIFF_REMOVED,
  THEME_DIFF_CONTEXT,
  THEME_DIFF_HUNK,
  THEME_DIFF_META,
  THEME_KEYWORD,
  THEME_TYPE,
  THEME_STRING,
  THEME_COMMENT,
  THEME_NUMBER,
  THEME_DIRECTIVE,
  THEME_FUNCTION,
  THEME_COUNT
} theme_role;
typedef struct theme_palette {
  char colours[THEME_COUNT][64];
} theme_palette;
void theme_defaults(theme_palette *palette);
void theme_use(const theme_palette *palette);
const char *theme_colour(theme_role role);
const char *theme_role_name(theme_role role);
int theme_set(theme_palette *palette, const char *name, const char *value);
struct abuf;
void theme_append(struct abuf *ab, theme_role role);
#endif
