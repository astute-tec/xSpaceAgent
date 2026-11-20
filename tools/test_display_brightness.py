#!/usr/bin/env python3
"""Exercise the real nonblocking brightness API and worker with SPI stubs."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1]/'firmware/wifi-provision/main/display.c').read_text()


def function(signature):
    start = source.index(signature+'\n{')
    opening = source.index('{', start)
    depth, end = 1, opening+1
    while depth:
        depth += (source[end] == '{')-(source[end] == '}')
        end += 1
    return source[start:end]


harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#define LCD_OPCODE_WRITE_CMD 2ULL
#define pdTRUE 1
#define ESP_OK 0
#define pdMS_TO_TICKS(x) (x)
static int s_requested_brightness=-1, s_io=1, s_flush_lock=1, s_trans_sem=2;
static bool s_dma_pending;
static uint32_t s_brightness_updates;
static jmp_buf end_iteration;
static bool lock_available=true, dma_ready=true, held, spi_fails, new_request;
static int sent, calls;
void display_brightness(uint8_t level);
static int xSemaphoreTake(int sem,int ticks) {
    if(sem==s_flush_lock) { if(!lock_available) return 0; held=true; return 1; }
    assert(held && sem==s_trans_sem); return dma_ready;
}
static void xSemaphoreGive(int sem) { assert(sem==s_flush_lock && held); held=false; }
static int esp_lcd_panel_io_tx_param(int io,uint32_t cmd,const uint8_t *data,unsigned size) {
    assert(held && !s_dma_pending && size==1); sent=*data; calls++;
    if(new_request) display_brightness(77);
    return spi_fails ? -1 : 0;
}
static void vTaskDelay(int ticks) { longjmp(end_iteration,1); }
'''
harness += function('void display_brightness(uint8_t level)')+'\n'
harness += function('static void brightness_task(void *arg)')+'\n'
harness += r'''
static void step(void) { if(!setjmp(end_iteration)) brightness_task(0); }
int main(void) {
    display_brightness(0); display_brightness(192);
    assert(calls==0 && s_requested_brightness==192);
    step(); assert(calls==1 && sent==192 && !held && s_requested_brightness==-1);
    display_brightness(0); lock_available=false;
    step(); assert(calls==1 && s_requested_brightness==0);
    lock_available=true; s_dma_pending=true; dma_ready=false;
    step(); assert(calls==1 && s_dma_pending && s_requested_brightness==0 && !held);
    dma_ready=true; step(); assert(calls==2 && sent==0 && !s_dma_pending);
    display_brightness(192); spi_fails=true;
    step(); assert(s_requested_brightness==192 && !held);
    new_request=true; step(); assert(s_requested_brightness==77 && !held);
    spi_fails=false; new_request=false; step(); assert(sent==77 && s_requested_brightness==-1);
    assert(s_brightness_updates==3);
    puts("PASS: callers never use SPI, latest request wins, flush lock serialization, DMA timeout and failed-send retry");
}
'''
with tempfile.TemporaryDirectory(prefix='display-brightness-') as td:
    tmp = Path(td)
    (tmp/'test.c').write_text(harness)
    subprocess.run(['clang', '-O1', '-fsanitize=address,undefined', str(tmp/'test.c'), '-o', str(tmp/'test')], check=True)
    subprocess.run([str(tmp/'test')], check=True)
