#!/usr/bin/env python3
"""Host-render the actual firmware UI with ASan/UBSan; export review PNGs.

Run from the repo root: .venv/bin/python tools/test_watchface_render.py
No device connection or recording writes are performed.
"""
import os
from pathlib import Path
import subprocess
import tempfile

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/wifi-provision/main"
OUT = ROOT / "out/answer-text-validation"
OUT.mkdir(parents=True, exist_ok=True)

STUBS = {
    "driver/gpio.h": "#pragma once\n",
    "driver/i2c_master.h": "#pragma once\ntypedef void *i2c_master_bus_handle_t;\n",
    "esp_err.h": "#pragma once\n#include <stdbool.h>\ntypedef int esp_err_t;\n",
    "esp_timer.h": "#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n",
    "freertos/FreeRTOS.h": """#pragma once
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(x) ((void)(x))
#define taskEXIT_CRITICAL(x) ((void)(x))
""",
}

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "display.h"
#include "watchface.h"
#include "font_cn.h"
static uint16_t frame[466*466];
static int64_t clock_ms;
uint16_t *display_fb(void) { return frame; }
int64_t esp_timer_get_time(void) { return clock_ms * 1000; }
uint32_t display_full_generation(void) { return 1; }
void display_fill(uint16_t c) { for (int i=0; i<466*466; i++) frame[i]=__builtin_bswap16(c); }
bool display_flush_rows(int a,int b) { assert(a>=0 && b<=466 && a<b); return true; }
void ui_fill_rect(int x,int y,int w,int h,uint16_t c) {
    for(int py=y;py<y+h;py++) for(int px=x;px<x+w;px++)
        if(px>=0 && px<466 && py>=0 && py<466) frame[py*466+px]=__builtin_bswap16(c);
}
static void dump(const char *name) {
    FILE *f=fopen(name,"wb"); assert(f);
    fprintf(f,"P6\n466 466\n255\n");
    for(int i=0;i<466*466;i++) {
        uint16_t c=__builtin_bswap16(frame[i]);
        unsigned char rgb[3]={(c>>11)*255/31,((c>>5)&63)*255/63,(c&31)*255/31};
        fwrite(rgb,1,3,f);
    }
    fclose(f);
}
int main(void) {
    watchface_set_status(true,-52,73,false,false);
    watchface_set_answer("今天以沟通准备为主：明天开会讨论来访人员要学的新内容，具体会上定；另外零散提了安卓和PC系统话题，还没结论。");
    assert(watchface_has_answer());
    watchface_set_recording(true);
    watchface_draw(WATCHFACE_TTS,0,0,0);
    dump("first.ppm");
    uint16_t first[466*466]; memcpy(first,frame,sizeof(frame));
    watchface_set_status(false,-127,-1,false,false);
    watchface_draw(WATCHFACE_TTS,0,0,0);
    dump("offline-battery-unknown.ppm");
    assert(memcmp(first+78*466,frame+78*466,48*466*2));
    assert(!memcmp(first+126*466,frame+126*466,(466-126)*466*2));
    watchface_set_status(true,-76,15,true,true);
    watchface_draw(WATCHFACE_TTS,0,0,0);
    dump("weak-wifi-charging.ppm");
    watchface_set_status(true,-52,73,false,false);
    watchface_draw(WATCHFACE_TTS,0,0,0);
    assert(!memcmp(first,frame,sizeof(frame)));
    clock_ms=7000;
    watchface_draw(WATCHFACE_RECORDING,0,0,0);
    dump("scroll.ppm");
    assert(memcmp(first+300*466,frame+300*466,84*466*2));
    assert(!memcmp(first,frame,300*466*2)); /* clock/date remain intact */
    /* Repeated status polling must not restart the scroll timer. */
    uint16_t scroll[466*466]; memcpy(scroll,frame,sizeof(frame));
    watchface_set_answer("今天以沟通准备为主：明天开会讨论来访人员要学的新内容，具体会上定；另外零散提了安卓和PC系统话题，还没结论。");
    watchface_draw(WATCHFACE_RECORDING,0,0,0);
    assert(!memcmp(scroll,frame,sizeof(frame)));
    clock_ms=999999;
    watchface_draw(WATCHFACE_PLAYING,0,0,0);
    char long_text[4001];
    for(int i=0;i<1000;i++) memcpy(long_text+i*4,"😀",4);
    long_text[4000]=0;
    watchface_set_answer(long_text);
    watchface_draw(WATCHFACE_ERROR,0,0,0);
    memset(long_text,'A',4000); long_text[4000]=0;
    watchface_set_answer(long_text);
    clock_ms+=500000;
    watchface_draw(WATCHFACE_IDLE,0,0,0);
    watchface_set_answer(0); assert(!watchface_has_answer());
    watchface_draw(WATCHFACE_RECORDING,0,0,0);
    /* Every common Chinese character in the real answer must draw pixels. */
    const char *chars="沟通准备明天开会讨论来访人员具体另外零散安卓系统话题结论王阿忠张文剑袁进坤";
    display_fill(0); ui_text(0,0,"?",0xffff,1);
    uint16_t question_glyph[24*466]; memcpy(question_glyph,frame,sizeof(question_glyph));
    for(const char *p=chars;*p;p+=3) {
        char ch[4]={p[0],p[1],p[2],0}; display_fill(0);
        ui_text(0,0,ch,0xffff,1);
        int lit=0; for(int i=0;i<24*466;i++) lit+=frame[i]!=0;
        assert(lit>0);
        assert(memcmp(question_glyph,frame,sizeof(question_glyph)));
    }
    puts("PASS: UTF-8 bounds, scrolling, clock preservation, polling, clear and glyph rendering");
}
'''

with tempfile.TemporaryDirectory(prefix="watchface-test-") as tmp:
    path = Path(tmp)
    for name, content in STUBS.items():
        target = path / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content)
    (path / "harness.c").write_text(HARNESS)
    subprocess.run(["clang", "-g", "-fsanitize=address,undefined", "-I", tmp,
                    "-I", str(MAIN), str(path / "harness.c"), str(MAIN / "watchface.c"),
                    str(MAIN / "font_cn.c"), "-lm", "-o", str(path / "render")], check=True)
    subprocess.run([str(path / "render")], cwd=path, check=True,
                   env={**os.environ, "TZ": "Asia/Shanghai"})
    for name in ("first", "scroll", "offline-battery-unknown", "weak-wifi-charging"):
        Image.open(path / (name + ".ppm")).save(OUT / (name + ".png"))
print(OUT)
