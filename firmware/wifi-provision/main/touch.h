#pragma once
#include "esp_err.h"
esp_err_t touch_init(void);
/* 返回 1=刚按下（边沿），0=无 */
int touch_took_press(void);
/* 返回 1=刚按下并带出坐标；0=无按下 */
int touch_press_xy(uint16_t *x, uint16_t *y);
/* 返回 1=当前按住（或刚松开），并带出坐标；down=1 按住 / 0 松开 */
int touch_poll_xy(uint16_t *x, uint16_t *y, int *down);
