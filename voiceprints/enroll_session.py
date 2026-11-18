#!/usr/bin/env python3
"""声纹录入：通过设备录每个人的跟读音频 → 提取声纹 → 存入声纹库。

用法：
    python3 voiceprints/enroll_session.py "文剑" "进坤" "忠哥"
"""
import subprocess, sys, time, os, json
from pathlib import Path

PORT = "/dev/cu.usbmodem1101"
OUT = Path(__file__).resolve().parents[1] / "voiceprints" / "audio"
OUT.mkdir(parents=True, exist_ok=True)

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "hub"))
from mywork_hub.voiceprint import enroll, list_speakers

NAMES = sys.argv[1:] if len(sys.argv) > 1 else ["文剑", "进坤", "忠哥"]

SCRIPT = [
    "大家好，我是{name}。今天我们在讨论项目的进展情况和后续安排。",
    "我们的方案是优先走UDP，如果中间的网关不支持就自动回退到TCP协议。",
    "下午我还要确认一下系统兼容性，把客户端的版本再测一遍。",
    "最后同步一下排期：这个月底之前出测试报告，下个月初完成交付。",
    "以上就是我的工作汇报，有什么问题请随时反馈。",
]

import serial

def record_person(name: str, ser: serial.Serial):
    """录一个人：依次读 SCRIPT 里的每段话"""
    print(f"\n=== {name} 声纹录入 ===")
    print("请对着设备说话（正常音量，距离 0.5-1 米）\n")

    segs = []
    for i, text in enumerate(SCRIPT):
        actual = text.replace("{name}", name)
        print(f"  [{i+1}/{len(SCRIPT)}] 请说: {actual[:30]}...")
        print("  (录音 6 秒)")

        # 清空串口缓冲
        ser.reset_input_buffer()

        # 开始录音
        ser.write(b"REC\n")
        time.sleep(0.5)

        # 用 Mac TTS 提示要说什么（帮助引导）
        subprocess.run(["say", "-v", "Tingting", actual], timeout=15)

        # 等说完再录 2 秒
        time.sleep(3)

        # 结束录音
        ser.write(b"REC\n")
        time.sleep(3)   # 等上传完成

        # 从 uploads 目录取最新的（最近 10 秒内生成的）
        import time as _t
        now = _t.time()
        recent = [f for f in OUT.parent.glob("uploads/*.wav")
                  if now - f.stat().st_mtime < 10]
        if recent:
            latest = max(recent, key=lambda f: f.stat().st_mtime)
            segs.append(str(latest))
            print(f"  ✓ 已录 {latest.stat().st_size // 1024} KB")
        else:
            print("  ✗ 没有录到新文件")

    return segs


def main():
    import serial
    ser = serial.Serial(PORT, 115200, timeout=0.3)
    time.sleep(1)

    all_ok = True
    for name in NAMES:
        segs = record_person(name, ser)
        if not segs:
            print(f"  ✗ {name} 没录到")
            all_ok = False
            continue

        # 提取声纹
        from mywork_hub.voiceprint import enroll
        result = enroll(name, segs)
        print(f"  结果: {json.dumps(result, ensure_ascii=False)}")

    ser.close()

    if all_ok:
        print(f"\n=== 已注册声纹: {', '.join(list_speakers())} ===")

if __name__ == "__main__":
    main()
