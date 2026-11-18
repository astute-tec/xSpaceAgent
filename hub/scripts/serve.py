#!/usr/bin/env python3
"""本地 hub：接收设备上传的 WAV，跑完整流水线。

    python3 hub/scripts/serve.py --port 8765 --root out/Mywork

设备端：POST /upload  (Content-Type: audio/wav)
"""
from __future__ import annotations

import argparse
import json
import sys
import threading
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mywork_hub import pipeline  # noqa: E402
from mywork_hub.config import cfg  # noqa: E402

UPLOAD_DIR = Path(__file__).resolve().parents[2] / "out" / "uploads"
LOCK = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # 静默默认日志
        pass

    def do_GET(self):
        if self.path == "/health":
            body = json.dumps({"ok": True, "root": str(cfg.root)}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            body = b"mywork-hub ready"
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    def do_POST(self):
        if self.path != "/upload":
            self.send_error(404)
            return
        length = int(self.headers.get("Content-Length", 0))
        if length <= 0:
            self.send_error(400, "empty body")
            return

        data = self.rfile.read(length)
        UPLOAD_DIR.mkdir(parents=True, exist_ok=True)
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        path = UPLOAD_DIR / f"{stamp}.wav"
        path.write_bytes(data)
        print(f"[hub] 收到 {len(data)} 字节 -> {path}", flush=True)

        self.send_response(200)
        body = b'{"ok":true}'
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

        threading.Thread(target=process, args=(path,), daemon=True).start()


def process(path: Path) -> None:
    if not LOCK.acquire(blocking=False):
        print("[hub] 上一个任务还在跑，排队处理", flush=True)
        LOCK.acquire()
    try:
        print(f"[hub] 开始处理 {path.name}", flush=True)
        meta = pipeline.ingest(path, keep_audio=False)
        print(
            f"[hub] 完成 {meta['session_id']} 段={meta['segments']} "
            f"保留={meta['kept']} 存疑={meta['review']}",
            flush=True,
        )
        from mywork_hub import report

        date = meta["date"]
        sessions = pipeline.load_day(date)
        daily = report.write_daily(date, sessions)
        print(f"[hub] 日报已更新：{daily.get('one_liner')}", flush=True)
    except Exception as exc:  # noqa: BLE001
        print(f"[hub] 处理失败：{exc}", flush=True)
    finally:
        LOCK.release()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--root", default=None)
    args = ap.parse_args()
    if args.root:
        cfg.root = Path(args.root).expanduser().resolve()
    print(f"[hub] 监听 0.0.0.0:{args.port}  产出根目录 {cfg.root}", flush=True)
    ThreadingHTTPServer(("0.0.0.0", args.port), Handler).serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
