# mywork-hub —— 语音工作助手 · 电脑端流水线

把一段录音变成一份能直接进 `~/Mywork` 的工作日报。**不依赖板子，也不依赖 6.6**，先在本机跑通。

## 链路

```
音频文件
  → ffmpeg 转 16k 单声道
  → whisper.cpp 转写（带时间戳）
  → DeepSeek 逐段判定：是不是工作内容
  → 丢弃闲聊 / 标记敏感
  → DeepSeek 汇总成日报（按项目、决议、风险、待办）
  → 写入 Mywork：daily.md / daily.json / tasks.json
```

## 用法

```bash
cd ~/ESP32Dev/hub

# 1. 处理一段录音（会转写 + 过滤 + 落盘）
PYTHONPATH=. python3 -m mywork_hub.cli ingest /path/to/meeting.wav

# 2. 生成当天日报
PYTHONPATH=. python3 -m mywork_hub.cli daily

# 看项目实体（自动从 Mywork 目录抽的）
PYTHONPATH=. python3 -m mywork_hub.cli entities
```

指定输出目录（测试时别污染真目录）：

```bash
PYTHONPATH=. python3 -m mywork_hub.cli ingest x.wav --root /tmp/mywork-test
PYTHONPATH=. python3 -m mywork_hub.cli daily --root /tmp/mywork-test
```

## 产出结构

```
<root>/05_语音工作日志/2026-11/2026-11-16/
  sessions/2026-11-16_1604_a016/
    audio16k.wav        16k 单声道（ASR 输入）
    source.wav          原始音频
    segments.jsonl      逐段：原文 + 判定 + 项目 + 待办
    transcript.md       人读全文（✅ 工作 / · 闲聊）
    meta.json           时长、段数、模型版本、保留数
    summary.json        单场结构化结果
  daily.md              人读日报
  daily.json            机器读日报
  tasks.json            待办（稳定 ID，供其他 agent 消费）
```

## 配置

从 `~/.config/mywork-hub/.env` 读：

| 变量 | 说明 |
|---|---|
| `DEEPSEEK_API_KEY` | 必需 |
| `LLM_MODEL_FAST` | 段级过滤，默认 `deepseek-flash` |
| `LLM_MODEL_PRO` | 日报汇总，默认 `deepseek-v4-pro` |
| `WHISPER_MODEL` | ggml 模型路径，默认 `~/.cache/whisper/ggml-medium.bin` |
| `FILTER_THRESHOLD` | 保留阈值，默认 0.6 |
| `MYWORK_ROOT` | Mywork 根目录，默认 `~/Mywork` |

## 设计取舍

| 决策 | 原因 |
|---|---|
| **两阶段过滤**：先规则、后 LLM | 纯 LLM 贵且慢；纯规则错杀多。段级批量送 LLM，8 段一次调用降本 |
| **实体词表自动生成** | 从 `Mywork/01_项目` 等目录抽项目/客户名喂给模型，项目归属准确率明显提升 |
| **日报两份**：md + json | 人看 md，agent 读 json，同源生成不会打架 |
| **原子写** | `tmp → fsync → rename`，避免 Syncthing 同步到半截文件 |
| **`voice:auto` 标记块** | 重生成只替换块内内容，不覆盖人写的东西 |

## 已知限制（v0）

- 没有说话人分离：`people` 是文本里提到的人名，不是"谁在说"
- 没有音频切段：整段音频一次转写，长会议要靠 whisper 自己的分段
- 云端 ASR 兜底未接（现在只有本地 whisper）
- 还没做推送唐僧（等 6.6 通）

## 下一步

1. 真机录音验证（用 ESP32 采的音频）
2. 说话人分离（3D-Speaker / CAM++）
3. 接云端 ASR 兜底（阿里 Paraformer + 项目热词）
4. 日报推送唐僧 + 写 `memory/YYYY-MM-DD.md`
