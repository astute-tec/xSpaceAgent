#!/usr/bin/env python3
"""Exercise actual recorder entry/cleanup functions with host hardware stubs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "firmware/wifi-provision/main/recorder.c").read_text()


def function(signature):
    start = source.index(signature + "\n{")
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


harness = r'''
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
static bool s_user_stopped,s_resume_after_ask,s_pending_start,s_recording,s_busy,s_asking;
static bool s_available,s_ptt_user_down,s_ptt_pending,s_ptt_active,s_ptt_capture_running;
static int s_ptt_len, starts, stop_during_delay, task_result=1, reset_ok=1;
static const char *TAG="test";
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static void vTaskDelay(int x) { if(stop_during_delay) s_user_stopped=true; }
static void recorder_start(void) { starts++; s_recording=true; s_busy=true; }
static bool menu_active(void) { return false; }
static void screen_wake(void) {}
static bool afe_reset_output(int x) { return reset_ok; }
static void ptt_capture_task(void *arg) {}
static int xTaskCreate(void(*fn)(void*), const char*n,int stack,void*arg,int prio,void*t) { return task_result; }
'''
harness += "\n".join(function(sig) for sig in (
    "static void ask_resume_recording(void)", "void recorder_stop(void)", "void recorder_ptt_begin(void)"))
harness += r'''
static void reset(void) {
    s_user_stopped=s_resume_after_ask=s_pending_start=s_recording=s_busy=s_asking=false;
    s_available=true; s_ptt_user_down=s_ptt_pending=s_ptt_active=s_ptt_capture_running=false;
    starts=stop_during_delay=0; task_result=reset_ok=1;
}
int main(void) {
    /* An idle-entry question must still restart the continuous recorder. */
    reset(); recorder_ptt_begin(); assert(s_asking && s_resume_after_ask);
    s_asking=false; ask_resume_recording(); assert(starts==1 && s_recording);
    /* The same cleanup handles cloud failure, silence and capture-task failure. */
    reset(); task_result=0; recorder_ptt_begin(); assert(starts==1 && s_recording);
    reset(); reset_ok=0; recorder_ptt_begin(); assert(starts==1);
    /* Recording-entry question seals the recording before acquiring the mic. */
    reset(); s_recording=s_busy=true; recorder_ptt_begin();
    assert(!s_recording && s_ptt_pending && s_resume_after_ask);
    s_busy=false; s_asking=false; ask_resume_recording(); assert(starts==1);
    /* An explicit stop, even during the delayed transition, always wins. */
    reset(); recorder_ptt_begin(); recorder_stop(); s_asking=false;
    ask_resume_recording(); assert(!starts && !s_recording);
    reset(); s_resume_after_ask=true; stop_during_delay=1;
    ask_resume_recording(); assert(!starts && !s_recording);
    puts("PASS: idle/recording entry, capture failure, AFE failure, explicit stop and stop race");
}
'''
with tempfile.TemporaryDirectory(prefix="recorder-test-") as tmp:
    path = Path(tmp)
    (path / "resume.c").write_text(harness)
    subprocess.run(["clang", "-fsanitize=address,undefined", str(path / "resume.c"),
                    "-o", str(path / "resume")], check=True)
    subprocess.run([str(path / "resume")], check=True)
