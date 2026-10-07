#ifndef ADM_PLUGIN_H
#define ADM_PLUGIN_H
#include <stddef.h>
#include <stdint.h>

#define ADM_PLUGIN_ABI_VERSION 1u
#ifdef _WIN32
#define ADM_PLUGIN_EXPORT __declspec(dllexport)
#else
#define ADM_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
// All positions are zero-based rows and UTF-8 byte columns.
// Context and API are borrowed for this synchronous command invocation only.
typedef struct adm_plugin_api {
  unsigned abi_version;
  size_t struct_size;
  const char *(*filename)(void *context);
  const char *(*workspace)(void *context);
  int (*cursor)(void *context, int *row, int *column);
  int (*line_count)(void *context);
  // Returns full byte length, or SIZE_MAX on failure. Copies at most capacity-1
  // bytes and terminates; NULL/0 queries length without copying the whole file.
  size_t (*read_line)(void *context, int row, char *out, size_t capacity);
  int (*replace)(void *context, int row, int column, int end_row,
                 int end_column, const char *text, size_t bytes);
  int (*set_cursor)(void *context, int row, int column);
  void (*notice)(void *context, const char *message);
} adm_plugin_api;
typedef int (*adm_plugin_command)(const adm_plugin_api *api, void *context);
// Each library exports this function and its configured command functions.
ADM_PLUGIN_EXPORT unsigned adm_plugin_version(void);
#ifdef __cplusplus
}
#endif
#endif
