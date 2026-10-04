#ifndef COMMAND_MENU_H
#define COMMAND_MENU_H

#include "dispatch.h"

int command_menu_capacity(const editor *e);
int command_menu_count(const editor *e);
void command_menu_scroll(editor *e, int step);
void command_menu_draw(editor *e, struct abuf *ab);

#endif
