#!/usr/bin/env python3
"""Run real touch/cancel/speaker functions with deterministic host event stubs."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
recorder = (root/'firmware/wifi-provision/main/recorder.c').read_text()
playback = (root/'firmware/wifi-provision/main/audio_playback.c').read_text()


def function(source, signature):
    start = source.index(signature+'\n{')
    end = source.index('{', start)+1
    depth = 1
    while depth:
        depth += (source[end] == '{')-(source[end] == '}')
        end += 1
    return source[start:end]


common = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define ESP_LOGI(...) ((void)0)
#define taskENTER_CRITICAL(x) ((void)0)
#define taskEXIT_CRITICAL(x) ((void)0)
'''
touch = common+r'''
typedef struct {int ms,x,y,down;} event_t;
static event_t events[12]; static int count,index_now,asks,wakes,opens,taps,longs;
static bool s_screen_off,s_recording,s_busy,s_asking,in_menu;
static jmp_buf ended;
static int64_t esp_timer_get_time(void) {return events[index_now].ms*1000LL;}
static void touch_poll_xy(uint16_t*x,uint16_t*y,int*d) {
    *x=events[index_now].x; *y=events[index_now].y; *d=events[index_now].down;
}
static void vTaskDelay(int n) {if(++index_now==count) longjmp(ended,1);}
bool menu_active(void) {return in_menu;}
void menu_open(void) {opens++;in_menu=true;}
bool menu_tap(int x,int y) {taps++;return true;}
bool menu_long_press(int x,int y) {longs++;return true;}
static void recorder_ask(void) {asks++;}
static void watchface_set_answer(const char*x) {}
static void screen_wake(void) {wakes++;}
static void show_idle(void) {}
'''+function(recorder,'static void touch_task(void *arg)')+r'''
static void run(event_t *e,int n) {
    memcpy(events,e,n*sizeof(*e));count=n;index_now=0;
    asks=wakes=opens=taps=longs=0;
    if(!setjmp(ended)) touch_task(NULL);
    assert(asks==0);
}
int main(void) {
    event_t hold[]={{0,230,240,1},{300,230,240,1},{2000,230,240,1},{2100,230,240,0}};
    run(hold,4);assert(!wakes);
    event_t dbl[]={{0,230,240,1},{60,230,240,0},{140,230,240,1},{210,230,240,0},{1000,0,0,0}};
    run(dbl,5);assert(wakes==1);
    event_t swipe[]={{0,230,50,1},{100,230,220,1},{200,230,220,0}};
    run(swipe,3);assert(opens==1);
    run(hold,4);assert(longs==1); /* Existing voiceprint-delete long press preserved. */
    in_menu=false;s_screen_off=true;
    run(dbl,5);assert(!wakes && !opens);
    puts("PASS: long/double touch never asks, swipe/menu long press retained, screen-off touch ignored");
}
'''

controls = common+r'''
static bool s_asking,s_ptt_pending,s_ptt_preparing,s_top_ptt_session,s_cancel_capture,s_wake_requested;
static bool s_ask_cancel,s_ptt_active,s_ptt_user_down,s_ptt_capture_running,s_playing;
static bool s_user_stopped,s_resume_after_ask,s_pending_start,s_recording,s_busy,s_available=true,in_menu;
static int s_top_clicks,s_ptt_len,starts,screen_toggles,muted,cleared,begins;
static int64_t s_error_until_ms;
typedef int* esp_http_client_handle_t;
static int lock_token,client_token,fd=-1,shutdowns,closes;
static bool lock_free=true;
static void *s_ask_client_lock=&lock_token;
static esp_http_client_handle_t s_ask_client;
#define portMAX_DELAY -1
#define SHUT_RDWR 2
static int xSemaphoreTake(void*l,int ticks) {if(!lock_free)return 0;lock_free=false;return 1;}
static void xSemaphoreGive(void*l) {assert(!lock_free);lock_free=true;}
static int esp_http_client_get_socket(esp_http_client_handle_t c) {assert(!lock_free && c==&client_token);return fd;}
static int shutdown(int socket,int how) {assert(!lock_free && socket==fd && how==2);shutdowns++;return 0;}
static void esp_http_client_close(esp_http_client_handle_t c) {assert(lock_free && s_ask_client==NULL);closes++;}
static void esp_http_client_cleanup(esp_http_client_handle_t c) {}
static void watchface_set_answer(const char*x) {assert(!*x);cleared++;}
static void mic_set_playback_vol(int n) {assert(n==0);muted++;}
static void vTaskDelay(int n) {}
static void recorder_start(void) {starts++;s_recording=s_busy=true;}
static void recorder_toggle_screen(void) {screen_toggles++;}
static bool menu_active(void) {return in_menu;}
static bool menu_voice_capture_active(void) {return false;}
static void recorder_ptt_begin(void) {begins++;s_asking=s_ptt_active=true;}
static int64_t s_top_last_ms;
static int64_t esp_timer_get_time(void) {return 1000000;}
static void vTaskDelete(void*t) {}
static void recorder_request_start(void) {if(!s_recording) recorder_start();}
'''
for sig in ('static void publish_question_client(esp_http_client_handle_t client)',
            'static void close_question_client(esp_http_client_handle_t client)',
            'static void cancel_question_http(void)', 'static void ask_resume_recording(void)',
            'bool recorder_question_active(void)', 'void recorder_ask(void)',
            'static void handle_wake_request(void)', 'void recorder_ask_cancel(void)',
            'static void cancel_capture_poll(void)', 'void recorder_bottom_key(void)',
            'static void top_click_task(void *arg)'):
    controls += function(recorder,sig)+'\n'
controls += r'''
static void reset(void) {
    s_asking=s_ptt_pending=s_ptt_preparing=s_top_ptt_session=s_cancel_capture=s_wake_requested=false;
    s_ask_cancel=s_ptt_active=s_ptt_user_down=s_ptt_capture_running=s_playing=false;
    s_user_stopped=s_resume_after_ask=s_pending_start=s_recording=s_busy=in_menu=false;
    s_top_clicks=s_ptt_len=starts=screen_toggles=muted=cleared=begins=0;
    s_ask_client=NULL;fd=-1;shutdowns=closes=0;lock_free=true;
}
int main(void) {
    reset();recorder_bottom_key();assert(screen_toggles==1 && !s_ask_cancel);
    /* Cancel active listening without blocking button handling or starting cloud IO. */
    reset();s_asking=s_top_ptt_session=s_ptt_active=s_ptt_user_down=s_ptt_capture_running=true;
    s_resume_after_ask=true;s_ptt_len=16000;s_top_clicks=2;
    recorder_bottom_key();assert(s_ask_cancel && s_cancel_capture && !s_ptt_active && !s_top_clicks && !screen_toggles);
    cancel_capture_poll();assert(!starts && s_ptt_len==16000);
    recorder_ask();assert(!begins); /* No buffer reuse while old capture owns it. */
    s_ptt_capture_running=false;cancel_capture_poll();assert(starts==1 && s_ptt_len==0 && !s_asking);
    recorder_ask();assert(begins==1 && !s_ask_cancel);
    /* A pending seal/reset must finish before cancel releases microphone ownership. */
    reset();s_ptt_pending=s_top_ptt_session=true;recorder_bottom_key();
    cancel_capture_poll();assert(s_cancel_capture);
    s_ptt_pending=false;s_ptt_preparing=true;cancel_capture_poll();assert(s_cancel_capture);
    s_ptt_preparing=false;cancel_capture_poll();assert(!s_cancel_capture);
    /* Waiting HTTP and playback get shutdown/mute; worker retains cleanup ownership. */
    reset();s_asking=s_playing=s_recording=s_busy=true;
    publish_question_client(&client_token);fd=42;recorder_bottom_key();
    assert(s_ask_cancel && shutdowns==1 && muted==1 && !s_cancel_capture && s_recording);
    recorder_ask();assert(!begins && s_ask_cancel);
    close_question_client(&client_token);assert(closes==1);
    cancel_question_http();assert(shutdowns==1); /* Detached handle cannot be reused. */
    /* Cancellation before socket assignment is retried once the owner connects. */
    reset();publish_question_client(&client_token);cancel_question_http();assert(!shutdowns);
    fd=43;cancel_question_http();assert(shutdowns==1);
    lock_free=false;cancel_question_http();assert(shutdowns==1);lock_free=true;
    close_question_client(&client_token);
    /* A queued wake is consumed once; an active cancelled session stays blocked. */
    reset();s_wake_requested=true;handle_wake_request();assert(begins==1 && !s_wake_requested);
    recorder_ask_cancel();recorder_ask();assert(begins==1 && s_ask_cancel);
    reset();s_top_clicks=1;top_click_task(NULL);assert(starts==1 && !begins);
    reset();s_top_clicks=2;top_click_task(NULL);assert(begins==1 && !starts);
    puts("PASS: listening/pending/network/playback cancel, socket lifetime, buffer ownership and recording resume");
}
'''

speaker = common+r'''
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
typedef struct { void *stream,*finished;bool mono;const volatile bool *cancelled;
                 volatile bool input_done,failed;int played;} playback_t;
static volatile bool cancelled;
static int receives,writes,mode;
static jmp_buf ended;
static void *heap_caps_malloc(size_t n,int caps) {return malloc(n);}
static size_t xStreamBufferReceive(void*s,void*b,size_t n,int ticks) {
    receives++;memset(b,0,n);if(mode==1)cancelled=true;return n;
}
static int mic_play(const void*b,int n) {writes++;cancelled=true;return n;}
static void xSemaphoreGive(void*s) {}
static void vTaskDelete(void*t) {longjmp(ended,1);}
'''+function(playback,'static void speaker_task(void *arg)')+r'''
int main(void) {
    for(mode=0;mode<3;mode++) {
        receives=writes=0;cancelled=mode==0;
        playback_t p={.cancelled=&cancelled};
        if(!setjmp(ended)) speaker_task(&p);
        assert(writes==(mode==2 ? 1 : 0));
        assert(receives==(mode==0 ? 0 : 1));
    }
    puts("PASS: cancelled speaker stops before next queued block; cancellation during receive suppresses playback");
}
'''

capture = common+r'''
static bool s_ready=true;static void *s_ring=(void *)1;
static volatile bool active=true;
static int mode,calls;
static uint8_t bytes[16];
static uint8_t *xRingbufferReceiveUpTo(void*r,size_t*n,int ticks,size_t need) {
    assert(ticks==50);calls++;
    if(mode==1 || (mode==2 && calls==2)) {active=false;return NULL;}
    *n=mode==2 ? 4 : need;return bytes;
}
static void vRingbufferReturnItem(void*r,void*p) {}
'''+function((root/'firmware/wifi-provision/main/afe.c').read_text(),
            'bool afe_read_while(int16_t *dest, int byte_count, const volatile bool *active)')+r'''
int main(void) {
    int16_t output[8];memset(bytes,0x12,sizeof(bytes));
    for(mode=0;mode<3;mode++) {
        active=true;calls=0;memset(output,0x7f,sizeof(output));
        bool ok=afe_read_while(output,sizeof(output),&active);
        assert(ok==(mode==0));assert(calls==(mode==2 ? 2 : 1));
        int copied=mode==0 ? 8 : (mode==2 ? 2 : 0);
        for(int i=0;i<8;i++) assert(output[i]==(i<copied ? 0x1212 : 0));
    }
    active=false;calls=0;assert(!afe_read_while(output,sizeof(output),&active));assert(!calls);
    puts("PASS: question read stops after one 50ms ring wait; partial buffers padded without overruns");
}
'''

button = common+r'''
#define REC_BUTTON 1
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
typedef struct {uint64_t pin_bit_mask;int mode,pull_up_en,pull_down_en,intr_type;} gpio_config_t;
static bool s_recording=true,s_busy=true,s_wake_requested,question;
static int s_stop_hold_progress,cancels,shorts,stops,step,mode;
static jmp_buf ended;
static void gpio_config(gpio_config_t*c) {}
static void axp_clear_key_irqs(void) {}
static int gpio_get_level(int pin) {return step==3;}
static int64_t esp_timer_get_time(void) {static int ms[]={0,100,3100,3200};return ms[step]*1000LL;}
static void vTaskDelay(int n) {if(n==600)return;if(++step==4)longjmp(ended,1);}
static bool recorder_question_active(void) {return question;}
static void recorder_ask_cancel(void) {cancels++;question=false;}
static void recorder_bottom_key(void) {shorts++;}
static void recorder_stop(void) {stops++;s_recording=false;}
static void screen_wake(void) {}
static void cancel_question_http(void) {}
static void cancel_capture_poll(void) {}
static void handle_wake_request(void) {}
static void axp_poll_key(void) {}
static bool menu_active(void) {return false;}
static void menu_poll(void) {}
static bool s_ask_cancel;
'''+function(recorder,'static void button_task(void *arg)')+r'''
int main(void) {
    question=true;
    if(!setjmp(ended)) button_task(NULL);
    assert(cancels==1 && !shorts && !stops && s_recording);
    puts("PASS: question cancelled on bottom press edge; continuing to hold cannot stop recording");
}
'''

send = function(recorder,'static void ask_send_task(void *arg)')
assert not re.search(r's_ask_cancel\s*=\s*false',send), 'Worker must not erase a prior cancellation'
assert 'recorder_ask(' not in function(recorder,'static void touch_task(void *arg)')

with tempfile.TemporaryDirectory(prefix='question-controls-') as td:
    path = Path(td)
    for name,source in [('touch',touch),('controls',controls),('speaker',speaker),
                        ('capture',capture),('button',button)]:
        (path/f'{name}.c').write_text(source)
        subprocess.run(['clang','-O1','-fsanitize=address,undefined',str(path/f'{name}.c'),'-o',str(path/name)],check=True)
        subprocess.run([str(path/name)],check=True)
