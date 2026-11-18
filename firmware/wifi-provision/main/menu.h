#pragma once
#include <stdbool.h>

void menu_init(void);
void menu_open(void);
void menu_close(void);
bool menu_active(void);
void menu_draw(void);
bool menu_tap(int x, int y);
bool menu_long_press(int x, int y);
void menu_poll(void);
void menu_set_slot(int slot, const char *name);
const char *menu_get_slot(int slot);
bool menu_load_slots(void);
void menu_save_slot(int slot);
