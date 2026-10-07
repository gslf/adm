#include "adm_plugin.h"
#include <time.h>

ADM_PLUGIN_EXPORT unsigned adm_plugin_version(void) {
  return ADM_PLUGIN_ABI_VERSION;
}
ADM_PLUGIN_EXPORT int insert_timestamp(const adm_plugin_api *api,
                                       void *context) {
  if (api->abi_version != ADM_PLUGIN_ABI_VERSION ||
      api->struct_size < sizeof *api)
    return -1;
  int row, column;
  if (api->cursor(context, &row, &column)) {
    api->notice(context, "Open or create a file before inserting a timestamp");
    return -1;
  }
  time_t now = time(NULL);
  struct tm *utc = gmtime(&now);
  char stamp[32];
  size_t bytes =
      utc ? strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", utc) : 0;
  return bytes ? api->replace(context, row, column, row, column, stamp, bytes)
               : -1;
}
