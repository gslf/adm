#ifndef ADM_CONFIG_H
#define ADM_CONFIG_H
#include "theme.h"
#define ADM_COMMAND_LIMIT 64
#define ADM_CONFIG_LINE 4096

typedef struct user_command {
  char key;
  char label[128], function[128];
  char *library;
} user_command;
typedef struct adm_config {
  theme_palette palette;
  user_command commands[ADM_COMMAND_LIMIT];
  int count;
  char error[256];
} adm_config;
// Missing configuration is a successful default configuration.
adm_config *config_load(void);
void config_free(adm_config *config);
#endif
