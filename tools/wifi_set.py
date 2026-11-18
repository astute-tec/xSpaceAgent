#!/usr/bin/env python3
"""
通过 USB 串口给设备配置 WiFi —— 绕过手机配网页。

用法：
    python3 tools/wifi_set.py "DYZN88"
    python3 tools/wifi_set.py "DYZN88" --password daye8888
    python3 tools/wifi_set.py "DYZN88" --port /dev/cu.usbmodem1101

不传 --password 时会交互式提示输入（不会留在 shell 历史里）。
"""
import argparse
import getpass
import sys
import time

import serial

DEFAULT_PORT = "/dev/cu.usbmodem1101"


def main() -> int:
    ap = argparse.ArgumentParser(description="给配网固件下发 WiFi 账号")
    ap.add_argument("ssid", help="WiFi 名称（区分大小写）")
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--password", help="不传则交互输入")
    ap.add_argument("--timeout", type=float, default=60)
    args = ap.parse_args()

    pwd = args.password
    if pwd is None:
        pwd = getpass.getpass(f"「{args.ssid}」的密码: ")
    if not pwd:
        print("密码为空，已退出", file=sys.stderr)
        return 2

    try:
        ser = serial.Serial(args.port, 115200, timeout=0.3)
    except Exception as exc:  # noqa: BLE001
        print(f"打不开串口 {args.port}: {exc}", file=sys.stderr)
        return 2

    time.sleep(2.0)
    ser.write(f"WIFI {args.ssid} {pwd}\n".encode())

    deadline = time.time() + args.timeout
    buf = b""
    result = 1
    while time.time() < deadline:
        buf += ser.read(8192)
        if b"WIFI_OK" in buf:
            result = 0
            break
        if b"WIFI_FAIL" in buf:
            result = 1
            break
    ser.close()

    text = buf.decode("utf-8", "replace")
    for line in text.splitlines():
        if any(k in line for k in ("CLI_", "WIFI_", "attempt", "CONNECTED", "got ip")):
            print(line.strip())

    if result != 0:
        print("\n连接失败。看 WIFI_FAIL reason=")
        print("  201 → 找不到这个 SSID（名字拼错/大小写不对/5GHz）")
        print("  15 / 204 → 密码错误")
        print("  202 → 认证方式不支持（WPA3/企业网）")
        print("  205 → 信号太弱")
    return result


if __name__ == "__main__":
    sys.exit(main())
