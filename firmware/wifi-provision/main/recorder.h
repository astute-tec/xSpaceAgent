#pragma once
#include <stdint.h>
#include "esp_err.h"

void recorder_init(const char *hub_ip, int hub_port);
void recorder_set_url(const char *url, const char *token);
void recorder_start(void);
void recorder_stop(void);
void recorder_request_start(void);
void recorder_top_key(void);   /* 上面键：设置页→回主界面 / 单击录音 / 双击问答 */  /* 上面键短按：仅开始录音 */
void recorder_ask(void);
void recorder_ask_wake(void);
void recorder_ask_cancel(void);
void recorder_bottom_key(void);         /* 问答取消；其他状态切换屏幕 */
void recorder_ptt_begin(void);          /* 仅授权的按键/唤醒词入口调用 */
void recorder_ptt_end(bool send);        /* 上键提交；取消丢弃缓冲 */
int recorder_grab_pcm(int16_t *buf, int max_bytes, int seconds);
bool recorder_is_busy(void);   /* 录音/问答/收尾中 */
bool recorder_question_active(void);
bool recorder_upload_enroll(const uint8_t *wav, int len, int slot, char *out, int outlen);
bool recorder_upload_unenroll(int slot);
bool recorder_upload_rename(int slot, const char *name);          /* 按键触发：开始/结束 */
const char *recorder_state(void);
void recorder_log_diagnostics(void);
bool recorder_auto_enabled(void);
void recorder_auto_set(bool on);
const char *recorder_upload_url(void);
const char *recorder_upload_token(void);
