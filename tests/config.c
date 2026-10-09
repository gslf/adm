#include "config.h"
#include "path.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
static void environment(const char *key, const char *value) {
  assert(!_putenv_s(key, value));
}
#else
#include <sys/stat.h>
#include <unistd.h>
static void environment(const char *key, const char *value) {
  assert(!setenv(key, value, 1));
}
#endif
static void write_text(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  assert(f);
  assert(fwrite(text, 1, strlen(text), f) == strlen(text));
  assert(!fclose(f));
}
static void rejected(const char *text) {
  write_text("profile.conf", text);
  adm_config *c = config_load();
  assert(c && c->error[0] && !c->count);
  theme_palette defaults;
  theme_defaults(&defaults);
  assert(!memcmp(&c->palette, &defaults, sizeof defaults));
  config_free(c);
}
int main(int argc, char **argv) {
  assert(argc == 2);
  (void)argv;
  adm_config *c = config_load();
  assert(c && !c->error[0] && !c->count);
  assert(!strcmp(c->palette.colours[THEME_STATUS], "\x1b[30;103m"));
#ifdef _WIN32
  assert(!strcmp(c->palette.colours[THEME_SELECTION], "\x1b[0;30;43m"));
  assert(!strcmp(c->palette.colours[THEME_ACTIVE], "\x1b[0;30;43m"));
#else
  assert(!strcmp(c->palette.colours[THEME_SELECTION], "\x1b[44;97m"));
  assert(!strcmp(c->palette.colours[THEME_ACTIVE], "\x1b[97;44m"));
#endif
  config_free(c);
  char *cwd = path_current_directory();
  assert(cwd);
  environment("HOME", cwd);
  environment("USERPROFILE", cwd);
  environment("ADM_CONFIG", "");
  write_text(".adm.conf", "\xef\xbb\xbf[colors]\r\nnormal = 252, 234\r\n");
  c = config_load();
  assert(c && !c->error[0]);
  assert(
      !strcmp(c->palette.colours[THEME_NORMAL], "\x1b[0;38;5;252;48;5;234m"));
  config_free(c);
  environment("ADM_CONFIG", "profile.conf");
  write_text("base.theme", "[colors]\nnormal = 1, 2\nkeyword = 3, 4\n");
  write_text("profile.conf",
             "# comment\n[colors]\nnormal = 252, 234\n[command t]\nlabel = "
             "Timestamp\nlibrary = ~/fixture library.so\nfunction = "
             "insert_timestamp\n[theme]\nfile = base.theme\n");
  c = config_load();
  assert(c && !c->error[0] && c->count == 1);
  assert(
      !strcmp(c->palette.colours[THEME_NORMAL], "\x1b[0;38;5;252;48;5;234m"));
  assert(!strcmp(c->palette.colours[THEME_KEYWORD], "\x1b[0;38;5;3;48;5;4m"));
  char *library = path_join(cwd, "fixture library.so");
  assert(library && !strcmp(c->commands[0].library, library));
  free(library);
  config_free(c);
  rejected("[colors]\nnormal = 256, 0\n");
  rejected("[colors]\nunknown = 1, 2\n");
  rejected("[colors]\nnormal = 1, 2\nnormal = 3, 4\n");
  rejected("[colors]\nnormal = -1, 2\n");
  rejected("[colors]\nnormal = 1;31m, 2\n");
  rejected("[colors]\nnormal = 1, 2\n[unknown]\n");
  rejected("[command t]\nlabel = incomplete\n");
  rejected("[command t]\nlabel = a\nlibrary = f.so\nfunction = a()\n");
  rejected("[command t]\nlabel = a\nlibrary = f.so\nfunction = a\n[command "
           "t]\nlabel = b\nlibrary = f.so\nfunction = b\n");
  rejected("[command t]\nlabel = a\nlabel = b\nlibrary = f.so\nfunction = a\n");
  rejected("[command  ]\n");
  rejected("[colors]\nnormal = 1, 2\n[theme]\nfile = absent.theme\n");
  write_text("base.theme", "[theme]\nfile = base.theme\n");
  rejected("[theme]\nfile = base.theme\n"); // Includes never recurse.
  write_text("base.theme",
             "[command t]\nlabel = injected\nlibrary = f.so\nfunction = a\n");
  rejected("[theme]\nfile = base.theme\n");
  char huge[4200];
  memset(huge, '#', sizeof huge - 1);
  huge[sizeof huge - 1] = 0;
  rejected(huge);
  FILE *f = fopen("profile.conf", "wb");
  assert(f);
  for (int i = 0; i < 70000; i++)
    fputc('\n', f);
  fclose(f);
  c = config_load();
  assert(c && c->error[0] && !c->count);
  config_free(c);
  f = fopen("profile.conf", "wb");
  assert(f);
  const char binary[] = "[colors]\nnormal = 1, 2\0hidden\n";
  fwrite(binary, 1, sizeof binary - 1, f);
  fclose(f);
  c = config_load();
  assert(c && c->error[0]);
  config_free(c);
#ifndef _WIN32
  assert(!mkfifo("config.pipe", 0600));
  environment("ADM_CONFIG", "config.pipe");
  c = config_load();
  assert(c && c->error[0]);
  config_free(c);
#endif
  environment("ADM_CONFIG", "profile.conf");
  f = fopen("profile.conf", "wb");
  assert(f);
  for (int i = 0; i < ADM_COMMAND_LIMIT; i++)
    fprintf(f,
            "[command %c]\nlabel = Command %d\nlibrary = "
            "literal$(cmd).so\nfunction = invoke\n",
            '!' + i, i);
  fclose(f);
  c = config_load();
  assert(c && !c->error[0] && c->count == ADM_COMMAND_LIMIT);
  assert(strstr(c->commands[0].library, "literal$(cmd).so"));
  config_free(c);
  f = fopen("profile.conf", "ab");
  assert(f);
  fprintf(f,
          "[command a]\nlabel = Overflow\nlibrary = f.so\nfunction = invoke\n");
  fclose(f);
  c = config_load();
  assert(c && c->error[0] && !c->count);
  config_free(c);
  const char *names[] = {
      "midnight",        "nord",    "dracula", "gruvbox", "solarized-dark",
      "solarized-light", "monokai", "ocean",   "forest",  "paper"};
  const char *themes = getenv("ADM_TEST_THEMES");
  assert(themes);
  environment("ADM_CONFIG", "profile.conf");
  for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
    char text[4096];
    snprintf(text, sizeof text, "[theme]\nfile = %s/%s.theme\n", themes,
             names[i]);
    write_text("profile.conf", text);
    c = config_load();
    assert(c && !c->error[0]);
    theme_use(&c->palette);
    for (int role = 0; role < THEME_COUNT; role++) {
      assert(strstr(theme_colour(role), "\x1b[0;38;5;"));
      assert(theme_role_name(role));
    }
    config_free(c);
  }
  theme_use(NULL);
  assert(!strcmp(theme_colour(THEME_TYPE), "\x1b[96m"));
  free(cwd);
  return 0;
}
