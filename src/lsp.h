#ifndef ADM_LSP_H
#define ADM_LSP_H
#include "language.h"
#include <stddef.h>
struct editor;
struct document;
struct module;
struct abuf;
struct module *lsp_module(void);
void lsp_tick(struct editor *e);
int lsp_busy(const struct editor *e);
int lsp_modal(const struct editor *e);
int lsp_key(struct editor *e, int key);
void lsp_detach(struct document *doc);
void lsp_saved(struct document *doc);
int lsp_version(const struct document *doc);
int lsp_cursor(const struct editor *e, int *x, int *y);
const char *lsp_status(const struct editor *e);
size_t lsp_diagnostic_count(const struct document *doc);
int lsp_position_units(const char *line, int bytes, int utf8);
int lsp_position_bytes(const char *line, int units, int utf8);
char *lsp_uri(const char *path);
char *lsp_uri_path(const char *uri);
#endif
