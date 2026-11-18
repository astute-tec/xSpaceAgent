#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* 官方 BluFi 蓝牙配网：
 * 手机装 "ESP Blufi" App（iOS/Android 官方应用）→ 连接设备 → 发 WiFi 账号密码。
 */
esp_err_t blufi_prov_init(void);
void blufi_prov_start(void);     /* 开始广播（仅配网模式调用） */
void blufi_prov_stop(void);      /* 停止并释放蓝牙 */
bool blufi_prov_running(void);
