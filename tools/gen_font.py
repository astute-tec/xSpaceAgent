#!/usr/bin/env python3
"""把界面需要的字符渲染成 1-bit 位图字库（C 数组），供固件显示中文。

用法: python3 tools/gen_font.py
产出: firmware/wifi-provision/main/font_cn.c / font_cn.h
"""
import re
from PIL import Image, ImageDraw, ImageFont
from pathlib import Path

CELL = 24          # 每个字符 24x24 像素
FONT_PATH = "/System/Library/Fonts/STHeiti Medium.ttc"

# 界面中所有会用到的字符（去重后生成）
# 自动收集：扫描固件源码里所有中文字符
import glob as _glob
TEXT_SAMPLES = []
_seen = set()
for _f in _glob.glob("firmware/wifi-provision/main/*.c"):
    for _m in re.findall(r'"([^"]*)"', open(_f, encoding="utf-8").read()):
        for _c in _m:
            if ord(_c) > 127 and _c not in _seen:
                _seen.add(_c)
                TEXT_SAMPLES.append(_c)

chars = set()
for s in TEXT_SAMPLES:
    chars.update(s)
# 完整 ASCII：SSID、URL、配对码都要能显示
chars.update(chr(c) for c in range(32, 127))
# Cloud answers are dynamic: menu-only glyphs would silently hide most Chinese.
# GB2312 covers 6763 common Han characters plus punctuation; keep glyphs in Flash.
for lead in range(0xA1, 0xF8):
    for trail in range(0xA1, 0xFF):
        try:
            chars.update(bytes((lead, trail)).decode("gb2312"))
        except UnicodeDecodeError:
            pass
chars.update("…“”‘’—·")
chars = sorted(chars)
print(f"字符数: {len(chars)}")

font = ImageFont.truetype(FONT_PATH, 22)
glyphs = {}
for ch in chars:
    img = Image.new("1", (CELL, CELL), 0)
    d = ImageDraw.Draw(img)
    bbox = d.textbbox((0, 0), ch, font=font)
    w = bbox[2] - bbox[0]
    h = bbox[3] - bbox[1]
    x = (CELL - w) // 2 - bbox[0]
    y = (CELL - h) // 2 - bbox[1]
    d.text((x, y), ch, font=font, fill=1)
    # 打包成 3 字节/行（24bit）
    data = bytearray()
    px = img.load()
    for row in range(CELL):
        bits = 0
        for col in range(CELL):
            if px[col, row]:
                bits |= (1 << (CELL - 1 - col))
        data += bits.to_bytes(CELL // 8, "big")
    glyphs[ch] = bytes(data)

out_h = Path("firmware/wifi-provision/main/font_cn.h")
out_c = Path("firmware/wifi-provision/main/font_cn.c")

out_h.write_text('''#pragma once
#include <stdint.h>

/* 自动生成：24x24 1-bit 中文字模（见 tools/gen_font.py） */
#define UI_FONT_CELL 24

/* 画一行 UTF-8 文本；scale=1/2；返回绘制宽度 */
int ui_text(int x, int y, const char *utf8, uint16_t color, int scale);
int ui_text_clipped(int x, int y, const char *utf8, uint16_t color, int scale, int top, int bottom);
''', encoding="utf-8")

lines = ['/* 自动生成：24x24 1-bit 字模，勿手改 */',
         '#include "font_cn.h"',
         '#include "display.h"',
         '#include <string.h>',
         '',
         'typedef struct { uint32_t cp; const uint8_t *data; } ui_glyph_t;',
         '',
         'static const uint8_t GLYPH_DATA[] = {']
for i, ch in enumerate(chars):
    data = glyphs[ch]
    hexs = ",".join(f"0x{b:02X}" for b in data)
    lines.append(f"  /* {ch} */ {hexs},")
lines.append('};')
lines.append('')
lines.append('static const ui_glyph_t GLYPHS[] = {')
for i, ch in enumerate(chars):
    lines.append(f"  {{ {ord(ch)}, GLYPH_DATA + {i} * {len(glyphs[ch])} }},")
lines.append('};')
lines.append(f'#define GLYPH_COUNT {len(chars)}')
lines.append('')
lines.append('''static const uint8_t *find_glyph(uint32_t cp)
{
    int lo = 0, hi = GLYPH_COUNT;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (GLYPHS[mid].cp == cp) return GLYPHS[mid].data;
        if (GLYPHS[mid].cp < cp) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

/* 解析一个 UTF-8 字符，返回消耗字节数 */
static int utf8_next(const char *s, uint32_t *cp)
{
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0) { *cp = ((c & 0x1F) << 6) | (s[1] & 0x3F); return 2; }
    if ((c & 0xF0) == 0xE0) { *cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); return 3; }
    if ((c & 0xF8) == 0xF0) { *cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); return 4; }
    *cp = '?';
    return 1;
}

int ui_text_clipped(int x, int y, const char *utf8, uint16_t color, int scale, int top, int bottom)
{
    int cx = x;
    int total = 0;
    while (*utf8) {
        uint32_t cp;
        int n = utf8_next(utf8, &cp);
        utf8 += n;
        const uint8_t *g = find_glyph(cp);
        if (!g) g = find_glyph('?');
        if (!g) { cx += UI_FONT_CELL * scale; total += UI_FONT_CELL * scale; continue; }
        int bytes_per_row = UI_FONT_CELL / 8;
        for (int row = 0; row < UI_FONT_CELL; row++) {
            for (int col = 0; col < UI_FONT_CELL; col++) {
                uint8_t byte = g[row * bytes_per_row + col / 8];
                if (byte & (1 << (7 - (col % 8)))) {
                    for (int sy = 0; sy < scale; sy++) {
                        for (int sx = 0; sx < scale; sx++) {
                            int py = y + row * scale + sy;
                            if (py >= top && py < bottom)
                                ui_fill_rect(cx + col * scale + sx, py, 1, 1, color);
                        }
                    }
                }
            }
        }
        cx += UI_FONT_CELL * scale;
        total += UI_FONT_CELL * scale;
    }
    return total;
}

int ui_text(int x, int y, const char *utf8, uint16_t color, int scale)
{
    return ui_text_clipped(x, y, utf8, color, scale, 0, 466);
}
''')
out_c.write_text("\n".join(lines), encoding="utf-8")
print(f"生成 {out_c} ({out_c.stat().st_size // 1024} KB)")
