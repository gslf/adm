#include "config.h"
#include "path.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char *trim(char *s) {
  while (*s == ' ' || *s == '\t')
    s++;
  char *end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' ||
                     end[-1] == '\n'))
    *--end = 0;
  return s;
}
static const char *home_directory(void) {
#ifdef _WIN32
  const char *home = getenv("USERPROFILE");
  if (home && *home)
    return home;
#endif
  return getenv("HOME");
}
static char *resolve(const char *base, const char *value) {
  char *joined;
  if (value[0] == '~' && (value[1] == '/' || value[1] == '\\')) {
    const char *home = home_directory();
    joined = home && *home ? path_join(home, value + 2) : NULL;
  } else if (value[0] == '/'
#ifdef _WIN32
             || value[0] == '\\' ||
             (isalpha((unsigned char)value[0]) && value[1] == ':')
#endif
  )
    joined = strdup(value);
  else
    joined = path_join(base, value);
  char *absolute = joined ? path_absolute(joined) : NULL;
  free(joined);
  return absolute;
}
static int symbol_name(const char *s) {
  if (!(isalpha((unsigned char)*s) || *s == '_'))
    return 0;
  while (*++s)
    if (!(isalnum((unsigned char)*s) || *s == '_'))
      return 0;
  return 1;
}
static int fail(adm_config *c, const char *path, int line, const char *reason) {
  snprintf(c->error, sizeof c->error, "Config %s:%d: %s", path_name(path), line,
           reason);
  return -1;
}
// Bounded files and lines, no interpolation, shell evaluation or recursive
// includes.
static int parse(adm_config *c, const char *path, int theme_only,
                 int optional) {
  struct stat info;
  if (stat(path, &info)) {
    if (optional && errno == ENOENT)
      return 0;
    return fail(c, path, 0, "cannot open file");
  }
  if (!S_ISREG(info.st_mode) || info.st_size > 65536)
    return fail(c, path, 0, "expected a regular file up to 64 KiB");
  FILE *file = fopen(path, "rb");
  if (!file) {
    if (optional && errno == ENOENT)
      return 0;
    return fail(c, path, 0, "cannot open file");
  }
  char *base = strdup(path);
  if (!base) {
    fclose(file);
    return fail(c, path, 0, "out of memory");
  }
  char *name = (char *)path_name(base);
  *name = 0;
  char line[ADM_CONFIG_LINE + 1], theme_file[ADM_CONFIG_LINE] = {0};
  unsigned char colour_seen[THEME_COUNT] = {0};
  theme_palette overrides;
  theme_defaults(&overrides);
  int section = 0, command = -1, lineno = 0, total = 0, result = 0;
  unsigned fields[ADM_COMMAND_LIMIT] = {0};
  while (!result) {
    int ch, used = 0;
    while ((ch = fgetc(file)) != EOF && ch != '\n') {
      if (++total > 65536 || used >= ADM_CONFIG_LINE - 1 || ch == 0 ||
          (ch < 32 && ch != '\t' && ch != '\r') || ch == 127) {
        result =
            fail(c, path, lineno + 1, "invalid or oversized configuration");
        break;
      }
      line[used++] = (char)ch;
    }
    if (result || (!used && ch == EOF))
      break;
    if (ch == '\n' && ++total > 65536) {
      result = fail(c, path, lineno + 1, "configuration too large");
      break;
    }
    lineno++;
    line[used] = 0;
    char *text = trim(
        lineno == 1 && used >= 3 && !memcmp(line, "\xef\xbb\xbf", 3) ? line + 3
                                                                     : line);
    if (!*text || *text == '#' || *text == ';')
      continue;
    if (*text == '[') {
      if (!strcmp(text, "[colors]"))
        section = 1;
      else if (!theme_only && !strcmp(text, "[theme]"))
        section = 2;
      else if (!theme_only && strlen(text) == 11 &&
               !strncmp(text, "[command ", 9) && text[10] == ']' &&
               text[9] > 32 && text[9] < 127) {
        if (c->count == ADM_COMMAND_LIMIT) {
          result = fail(c, path, lineno, "too many commands");
          break;
        }
        for (int i = 0; i < c->count; i++)
          if (c->commands[i].key == text[9])
            result = -1;
        if (result) {
          result = fail(c, path, lineno, "duplicate command key");
          break;
        }
        command = c->count++;
        c->commands[command].key = text[9];
        section = 3;
      } else {
        result = fail(c, path, lineno, "unknown section");
        break;
      }
      continue;
    }
    char *equals = strchr(text, '=');
    if (!equals) {
      result = fail(c, path, lineno, "expected key = value");
      break;
    }
    *equals++ = 0;
    char *key = trim(text), *value = trim(equals);
    if (!*value) {
      result = fail(c, path, lineno, "empty value");
      break;
    }
    if (section == 1) {
      int role = theme_set(&overrides, key, value);
      if (role < 0 || colour_seen[role])
        result = fail(c, path, lineno, "invalid or duplicate colour");
      else
        colour_seen[role] = 1;
    } else if (section == 2 && !strcmp(key, "file") && !*theme_file) {
      if (strlen(value) >= sizeof theme_file)
        result = fail(c, path, lineno, "theme path too long");
      else
        strcpy(theme_file, value);
    } else if (section == 3) {
      user_command *cmd = &c->commands[command];
      unsigned field = !strcmp(key, "label")      ? 1
                       : !strcmp(key, "library")  ? 2
                       : !strcmp(key, "function") ? 4
                                                  : 0;
      if (!field || (fields[command] & field))
        result = fail(c, path, lineno, "unknown or duplicate command setting");
      else if (field == 2) {
        cmd->library = resolve(base, value);
        if (!cmd->library)
          result = fail(c, path, lineno, "cannot resolve library path");
      } else if (strlen(value) >= 128 || (field == 4 && !symbol_name(value)))
        result = fail(c, path, lineno, "invalid command label or function");
      else
        strcpy(field == 1 ? cmd->label : cmd->function, value);
      fields[command] |= field;
    } else
      result = fail(c, path, lineno, "unknown setting");
    if (ch == EOF)
      break;
  }
  if (!result && ferror(file))
    result = fail(c, path, lineno, "read failed");
  fclose(file);
  if (!result && !theme_only) {
    for (int i = 0; i < c->count; i++)
      if (fields[i] != 7) {
        result =
            fail(c, path, lineno, "command needs label, library and function");
        break;
      }
  }
  if (!result && *theme_file) {
    char *theme = resolve(base, theme_file);
    result = theme ? parse(c, theme, 1, 0)
                   : fail(c, path, lineno, "cannot resolve theme path");
    free(theme);
  }
  if (!result)
    for (int i = 0; i < THEME_COUNT; i++)
      if (colour_seen[i])
        strcpy(c->palette.colours[i], overrides.colours[i]);
  free(base);
  return result;
}
void config_free(adm_config *c) {
  if (!c)
    return;
  for (int i = 0; i < c->count; i++)
    free(c->commands[i].library);
  free(c);
}
adm_config *config_load(void) {
  adm_config *c = calloc(1, sizeof *c);
  if (!c)
    return NULL;
  theme_defaults(&c->palette);
  const char *override = getenv("ADM_CONFIG"), *home = home_directory();
  char *path = override && *override ? path_absolute(override)
               : home && *home       ? path_join(home, ".adm.conf")
                                     : NULL;
  if (path && parse(c, path, 0, 1)) {
    // Reject the complete configuration: never run a partly parsed command.
    for (int i = 0; i < c->count; i++)
      free(c->commands[i].library);
    memset(c->commands, 0, sizeof c->commands);
    c->count = 0;
    theme_defaults(&c->palette);
  } else if (!path && override && *override)
    fail(c, override, 0, "cannot resolve configuration path");
  free(path);
  return c;
}
