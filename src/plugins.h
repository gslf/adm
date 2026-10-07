#ifndef ADM_PLUGINS_H
#define ADM_PLUGINS_H
struct editor;
struct module;
struct module *plugins_module(void);
int plugins_modal(const struct editor *e);
int plugins_key(struct editor *e, int key);
int plugins_count(const struct editor *e);
#endif
