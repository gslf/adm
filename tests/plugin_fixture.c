#include "adm_plugin.h"
#include <string.h>
#ifndef ADM_FIXTURE_NO_VERSION
ADM_PLUGIN_EXPORT unsigned adm_plugin_version(void) {
#ifdef ADM_FIXTURE_BAD_VERSION
  return 999;
#else
  return ADM_PLUGIN_ABI_VERSION;
#endif
}
#endif
ADM_PLUGIN_EXPORT int edit_shared(const adm_plugin_api *api, void *context) {
  if (api->abi_version != ADM_PLUGIN_ABI_VERSION ||
      api->struct_size < sizeof *api)
    return -1;
  if (!api->filename(context) || !api->workspace(context))
    return -1;
  if (api->replace(context, 0, 0, 0, 0, "plugin", 6))
    return -1;
  int r, c;
  if (api->cursor(context, &r, &c) || r || c != 6)
    return -1;
  return api->replace(context, r, c, r, c, "\nshared", 7);
}
ADM_PLUGIN_EXPORT int inspect_lines(const adm_plugin_api *api, void *context) {
  char tiny[4];
  if (api->line_count(context) != 2 ||
      api->read_line(context, 0, NULL, 0) != 6 ||
      api->read_line(context, 0, tiny, sizeof tiny) != 6 ||
      strcmp(tiny, "plu") ||
      api->read_line(context, -1, tiny, sizeof tiny) != SIZE_MAX ||
      api->read_line(context, 2, tiny, sizeof tiny) != SIZE_MAX ||
      api->read_line(context, 0, NULL, 1) != SIZE_MAX ||
      api->replace(context, 0, 99, 0, 99, "x", 1) != -1 ||
      api->replace(context, -1, 0, 0, 0, "x", 1) != -1 ||
      api->replace(context, 0, 0, 0, 0, "\0", 1) != -1 ||
      api->set_cursor(context, 1, 6))
    return -1;
  api->notice(context, "Plugin inspected shared lines");
  return 0;
}
ADM_PLUGIN_EXPORT int fail_after_edit(const adm_plugin_api *api,
                                      void *context) {
  if (api->replace(context, 0, 0, 0, 0, "x", 1))
    return -1;
  return 7;
}
ADM_PLUGIN_EXPORT int invalid_unicode_position(const adm_plugin_api *api,
                                               void *context) {
  if (api->set_cursor(context, 0, 1) != -1 ||
      api->replace(context, 0, 1, 0, 1, "x", 1) != -1)
    return -1;
  api->notice(context, "UTF-8 positions protected");
  return 0;
}
