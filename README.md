# xSpaceAgent · ESP32-S3 办公室语音工作助手

一块 **Waveshare ESP32-S3-Touch-AMOLED-1.75C**（466×466 圆屏 AMOLED）做成的桌面语音助手：
**自动听办公室对话 → 转文字 → 只留工作相关内容 → 按人/项目归类 → 每天汇总成日报推送给指定 Agent（"唐僧"）**，
并支持 **按键/唤醒语音问答**、**触摸设置菜单**、**蓝牙配网**、**服务端声纹区分谁在说话**。

```
┌──────────────┐   WiFi/HTTPS    ┌──────────────────┐   阿里 Paraformer   ┌────────────┐
│  ESP32-S3    │ ──────────────▶ │  Relay 中继服务   │ ─────────────────▶ │  转写文本  │
│  录音+AFE    │   /v1/upload    │  (Python Hub)    │                    └─────┬──────┘
│  降噪+AGC    │   /v1/ask       │                  │   DeepSeek 过滤/归类     │
│  唤醒+缓存   │ ◀────────────── │  /v1/enroll      │ ◀───────────────────────┘
│  圆屏 UI     │   TTS PCM       │  /v1/rename      │
└──────────────┘                 └────────┬─────────┘
                                          │ 日报 + 语音纪要
                                          ▼
                                 ┌──────────────────┐
                                 │  Agent "唐僧"     │
                                 │  (OpenClaw)      │
                                 └──────────────────┘
```

---

## 仓库结构

```
firmware/wifi-provision/     ESP-IDF 固件（本项目核心）
  main/
    main.c                   启动、WiFi 管理（多网络记忆/自动重连）、配网热点+HTTP、串口 CLI
    recorder.c               录音分段落盘、按住说话(PTT)、语音问答、按键/触摸交互、UI
    afe.c                    ESP-SR AFE：降噪 + AGC + 唤醒词("你好小智")，当前关闭 VAD/AEC
    queue.c                  Flash 安全队列：离线缓存、校验回执后释放、断网重传、声纹样本重试
    recording_codec.c        独立 worker：20 kbps CBR Ogg Opus、20 秒分段
    audio_playback.c         有界缓冲，边接收单声道 PCM 边播放
    watchface.c              黑色表盘、真实音量波形、滚动回答、WiFi/电量
    menu.c                   触摸设置菜单：WiFi 列表 / 新配二维码 / 声纹增删
    display.c                AMOLED 驱动（QSPI）+ 双缓冲/分带传输
    font_cn.c                24×24 点阵中文字库（tools/gen_font.py 生成）
    blufi_prov.c             官方 BluFi 蓝牙配网（NimBLE host）
    mic.c / touch.c / board.h  ES7210 双麦 + ES8311、CST9217 触摸、板级引脚
    web/index.html           扫码配网页面（手机浏览器）
  partitions.csv             分区表（25MB FATFS 录音区 + 2MB 唤醒模型区）
  sdkconfig.defaults         关键配置（PSRAM / AFE / NimBLE / FATFS）

hub/                         服务端（Python）
  mywork_hub/
    pipeline.py              音频入口：转写 → 说话人识别 → 过滤 → 归类 → 落盘
    asr_aliyun.py            阿里 Paraformer 一句话识别（分片+静音切分+热词表）
    filter.py                DeepSeek 两阶段过滤（只留工作内容 + 纠错）
    report.py                日报生成（双视图 + 沟通节奏统计）
    voiceprint_np.py         纯 numpy 声纹（MFCC 统计量，余弦相似度）
    push.py                  推送"唐僧"（写 memory 片段 + openclaw agent 通知）
    ask.py                   ASR 原问题 → 唐僧 → TTS；唐僧不可用时才降级 DeepSeek
    inbox.py                 SQLite 持久任务、去重/重启恢复/重试、日报窗口合并
    config.py / store.py / lexicon.py / hotwords.py
  scripts/
    public_relay.py          公网中继：/v1/upload /v1/ask /v1/enroll /v1/unenroll /v1/rename
    serve.py                 本地调试服务
    daily_job.py             定时日报任务（18:30）

deploy/                      部署到 Linux 服务器（systemd + Cloudflare Tunnel）
docs/                        方案设计、进展记录、问题复盘
tools/gen_font.py            生成中文字库 C 数组
tools/wifi_set.py            串口 WiFi 配置小工具
```

## 快速开始

### 1. 编译固件

```bash
# 需要 ESP-IDF v6.1（含 esp-sr / esp_codec_dev 组件管理器依赖）
source $IDF_PATH/export.sh
cd firmware/wifi-provision
idf.py set-target esp32s3
idf.py build
```

首次烧写会写入分区表 + 模型分区（`srmodels.bin`，含"你好小智"唤醒词）。

已有设备只升级应用分区，保留 WiFi、模型、声纹和待传录音，详见[烧录与离线验收](docs/固件烧录与设备离线验收_20261119.md)。当前候选版本为 `2026.11.19-rc1`，尚未完成两小时实机耐久验收。

### 2. 部署服务端

```bash
pip install -r hub/requirements.txt        # 见文件内注释
export MYWORK_ROOT=~/myWork
export MYWORK_SSH_HOST=user@your-server    # 推送目标（可选）
export DEVICE_TOKEN=$(openssl rand -hex 24)
export ALIYUN_ACCESS_KEY_ID=... ALIYUN_ACCESS_KEY_SECRET=... ALIYUN_NLS_APPKEY=...
export DEEPSEEK_API_KEY=...
python3 hub/scripts/public_relay.py --port 8787
```

### 3. 设备配网

三种方式（任选）：
1. **蓝牙**：手机装官方 "ESP Blufi" App → 设备下滑 → WiFi → 新配WiFi → App 连接 `Xiaozhi-xxxx` 输入 WiFi 密码
2. **扫码**：手机相机扫屏幕二维码（自动加入设备热点）→ 浏览器打开配网页选 WiFi
3. **串口**：`WIFI <ssid> <password>`

配网成功后凭据写入 NVS，开机自动重连（多网络记忆 + 失败回退 + 指数退避）。

## 设备交互

| 操作 | 行为 |
|---|---|
| 上面键短按 | 设置页先回表盘；待机时开始录音；录音中再按无效；聆听问题时提交问题 |
| 上面键双击 | 进入问题聆听，和“你好小智”唤醒一致 |
| 下面键短按 | 熄屏/亮屏（录音不受影响） |
| 下面键长按 3 秒 | 显示进度条，保存并停止录音 |
| 屏幕轻点 | 唤亮屏幕、清除回答显示 |
| **屏幕长按** | 进入问题聆听，松屏后继续听；说完按一次上面键提交，回答文字滚动显示并语音播报 |
| 屏幕顶部下滑 | 设置菜单（WiFi / 声纹） |
| 说「你好小智」 | 唤醒语音问答 |

录音默认 20 秒一段以 Ogg Opus 保存，包含静音和短尾段；联网补传，服务端持久化且设备核对回执后才释放本地副本。满盘不覆盖待传录音，挂载失败不自动格式化。两小时容量预算约 19.69 MiB，实际可录时长取决于设备空闲空间；两小时实录、电池续航、降噪/声纹质量仍待现场验收。当前 AEC 未启用，扬声器播报期间仍有工作录音空档。

## 串口命令（调试）

```
STATUS              连接/录音状态
SCAN / LISTWIFI     扫描周围 WiFi / 列出记忆的网络
CONNECT <idx>       按序号连接记忆的网络
SAVEWIFI <ssid> [pass]  只保存不连接
FORGET              清除 WiFi 凭据
HUBURL <url> [token]    设置上传地址
PROV / PROVSTOP     进入/退出配网模式（热点 + 二维码 + 蓝牙）
START / STOP / REC  开始 / 停止 / 开关录音
MENU / TAP x y      打开菜单 / 模拟点击
NAME <slot> <名字>   声纹改名（同步服务器）
UNENROLL <slot>     删除声纹
STORAGE            空间、保留文件、按实际空闲量估算离线秒数
CLEAN / CLEAR      请求安全补传；只释放收到完整匹配回执的副本
QUEUELS            列出队列文件
HEAP                查看内部 RAM / PSRAM
```

## 文档

- [录音优先架构实施与验证记录](docs/录音优先架构落地与两小时离线验收_20261119.md)
- [候选固件烧录与离线验收](docs/固件烧录与设备离线验收_20261119.md)
- [产品方案](docs/ESP32语音工作助手_产品方案.md)
- [工程设计](docs/工作助手_方案设计_v0.1.md)
- [WiFi 配网指南](docs/WiFi配网操作指南.md)
- [声纹录入指南](docs/声纹录入与识别修复_2026-11-18.md)
- [进展记录](docs/进展更新-2026-11-18.md)

## 许可

内部项目，未附许可；如需开源请先确认第三方依赖（ESP-IDF / ESP-SR / 阿里云 SDK）的许可条款。
