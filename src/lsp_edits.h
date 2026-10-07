#ifndef ADM_LSP_EDITS_H
#define ADM_LSP_EDITS_H
#include "json.h"
struct editor;
typedef struct lsp_edit_version {
  const char *uri;
  unsigned long long generation;
} lsp_edit_version;
// Applies only text edits. Unopened affected files become visible, unsaved
// tabs.
int lsp_apply_edits(struct editor *e, const json_value *v, size_t result,
                    const char *format_uri, int utf8,
                    const lsp_edit_version *versions, size_t count);
#endif
