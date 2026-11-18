#pragma once
#include <stdint.h>

/* 自动生成：24x24 1-bit 中文字模（见 tools/gen_font.py） */
#define UI_FONT_CELL 24

/* 画一行 UTF-8 文本；scale=1/2；返回绘制宽度 */
int ui_text(int x, int y, const char *utf8, uint16_t color, int scale);
