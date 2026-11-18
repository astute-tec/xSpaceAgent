#!/usr/bin/env python3
"""自动声纹录入：按顺序录多个人，每人在设备前跟读 → 提取声纹。

用法：
    python3 voiceprints/enroll_auto.py 文剑 售前 进坤 销售
    （名字 角色 名字 角色 ... 交替）

设备需要通过 USB 连接。
"""
import json
import struct
import sys
import time
from pathlib import Path

import serial
import numpy as np

PORT = "/dev/cu.usbmodem1101"
OUT_DIR = Path(__file__).resolve().parents[1] / "voiceprints" / "audio"
LIB_FILE = Path(__file__).resolve().parents[1] / "voiceprints" / "library.json"
OUT_DIR.mkdir(parents=True, exist_ok=True)

SCRIPT = [
    "大家好，我是{name}。今天我们在讨论项目的进展情况和后续安排。",
    "我们的方案是优先走UDP协议，如果网关不支持就自动回退到TCP。",
    "下午我还要确认一下系统的兼容性，把客户端的版本再测试一遍。",
    "最后同步一下排期：这个月底之前完成测试报告，下个月初交付。",
    "以上就是我的工作汇报，请随时反馈问题。",
]


def open_serial():
    import serial
    return serial.Serial(PORT, 115200, timeout=0.3)


def read_serial_for(ser, seconds):
    """读串口 N 秒，返回全部输出"""
    buf = bytearray()
    t0 = time.time()
    while time.time() - t0 < seconds:
        d = ser.read(4096)
        if d:
            buf.extend(d)
    return buf.decode("utf-8", "replace")


def record_segment(ser, seconds: float, label: str) -> bool:
    """触发设备录一段，返回是否成功"""
    ser.write(b"REC\n")
    time.sleep(0.3)
    ser.reset_input_buffer()
    time.sleep(seconds)
    ser.write(b"REC\n")   # 结束
    time.sleep(2)          # 等上传

    output = read_serial_for(ser, 2)
    return "queued segment" in output or "upload" in output


def main():
    args = sys.argv[1:]
    if len(args) < 2 or len(args) % 2 != 0:
        print("用法: enroll_auto.py 名字1 角色1 [名字2 角色2 ...]")
        print("例:   enroll_auto.py 文剑 售前 进坤 销售")
        sys.exit(1)

    people = []
    for i in range(0, len(args), 2):
        people.append((args[i], args[i + 1]))

    print(f"=== 声纹录入：{len(people)} 人 ===\n")

    ser = open_serial()
    time.sleep(1)

    # 导入声纹模块
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "hub"))
    from mywork_hub.voiceprint import enroll, list_speakers

    results = {}
    for name, role in people:
        print(f"\n{'='*40}")
        print(f"  {name}（{role}）开始录入")
        print(f"  请站在设备前 0.5-1 米，正常音量说话")
        print(f"{'='*40}\n")

        segs = []
        for i, text in enumerate(SCRIPT):
            actual = text.replace("{name}", name)
            print(f"  [{i+1}/{len(SCRIPT)}] 请说: {actual[:30]}...")
            time.sleep(1)   # 给准备时间

            ok = record_segment(ser, 6.0, f"{name}_seg{i}")
            if ok:
                # 找最新的 uploads 文件
                uploads = sorted(
                    Path(__file__).resolve().parents[1].glob("uploads/*.wav"),
                    key=lambda f: f.stat().st_mtime
                )
                if uploads:
                    segs.append(str(uploads[-1]))
                    print(f"      ✓ 已录 {uploads[-1].stat().st_size // 1024} KB")
            else:
                print(f"      ✗ 未检测到新录音")

        if segs:
            result = enroll(name, segs)
            print(f"\n  结果: {json.dumps(result, ensure_ascii=False)}")
            results[name] = result
        else:
            print(f"\n  ✗ {name} 录入失败")
            results[name] = {"ok": False}

    ser.close()

    print(f"\n{'='*40}")
    print("=== 录入完成 ===")
    for name, r in results.items():
        status = "✓" if r.get("ok") else "✗"
        print(f"  {status} {name}")


if __name__ == "__main__":
    main()
