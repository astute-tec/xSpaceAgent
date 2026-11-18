#!/usr/bin/env python3
"""公网接入服务：接收设备上传的录音，转写过滤后推送给唐僧。

部署形态：
    设备 --HTTPS--> Cloudflare (xiaozhi.halmir.top)
                        │ cloudflared 隧道
                        ▼
                   本服务 (:8787)  --本地--> OpenClaw「唐僧」

安全：Bearer token + 大小限制 + 简单限流；只接受 audio/*。
"""
from __future__ import annotations

import argparse
import hmac
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mywork_hub import pipeline, push, report  # noqa: E402
from mywork_hub import ask as askmod  # noqa: E402
from mywork_hub.asr_aliyun import transcribe_file  # noqa: E402
from mywork_hub.config import cfg  # noqa: E402

MAX_BYTES = int(os.environ.get("MAX_UPLOAD_MB", "32")) * 1024 * 1024
DEVICE_TOKEN = os.environ.get("DEVICE_TOKEN", "")
OPENCLAW_AGENT = os.environ.get("OPENCLAW_AGENT", "tangseng")
INBOX = Path(os.environ.get("INBOX_DIR", Path.home() / "myWork" / "05_语音工作日志" / "_inbox"))
DEPLOY_DIR = Path(os.environ.get("DEPLOY_DIR", Path(__file__).resolve().parents[2] / "deploy" / "pkg"))
PROCESS_LOCK = threading.Lock()
INBOX_LOCK = threading.Lock()
RATE: dict[str, list[float]] = {}

START = time.time()


def log(msg: str) -> None:
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {msg}", flush=True)


def rate_ok(ip: str) -> bool:
    now = time.time()
    hits = [t for t in RATE.get(ip, []) if now - t < 60]
    RATE[ip] = hits + [now]
    return len(RATE[ip]) <= 30           # 每 IP 每分钟 30 次


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "mywork-relay/1.0"

    def log_message(self, fmt, *args):
        pass

    def _json(self, code: int, obj: dict) -> None:
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/deploy/"):
            name = self.path[len("/deploy/"):]
            target = (DEPLOY_DIR / name).resolve()
            if DEPLOY_DIR.resolve() in target.parents and target.is_file():
                data = target.read_bytes()
                self.send_response(200)
                self.send_header("Content-Type", "text/x-shellscript" if target.suffix == ".sh"
                                 else "application/octet-stream")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
            else:
                self._json(404, {"ok": False, "msg": "not found"})
            return
        if self.path == "/health":
            self._json(200, {
                "ok": True,
                "uptime_s": int(time.time() - START),
                "inbox": str(INBOX),
                "agent": OPENCLAW_AGENT,
            })
        else:
            self._json(404, {"ok": False})

    def do_POST(self):
        if self.path == "/v1/ask":
            self._do_ask()
            return
        if self.path == "/v1/enroll":
            self._do_enroll()
            return
        if self.path == "/v1/unenroll":
            self._do_unenroll()
            return
        if self.path == "/v1/rename":
            self._do_rename()
            return
        self._do_upload()

    def _do_enroll(self):
        """声纹录入：临时处理音频，只持久化 embedding。"""
        ip = self.client_address[0]
        if not rate_ok(ip):
            self._json(429, {"ok": False, "msg": "too many requests"})
            return
        auth = self.headers.get("Authorization", "")
        token = auth.removeprefix("Bearer ").strip()
        if not DEVICE_TOKEN or not hmac.compare_digest(token, DEVICE_TOKEN):
            self._json(401, {"ok": False, "msg": "unauthorized"})
            return
        try:
            length = int(self.headers.get("Content-Length", 0))
            slot = int(self.headers.get("X-Slot", "0"))
        except ValueError:
            self._json(400, {"ok": False, "msg": "bad length or slot"})
            return
        if length <= 1000 or length > MAX_BYTES:
            self._json(400, {"ok": False, "msg": "bad size"})
            return
        if not (0 <= slot <= 4):
            self._json(400, {"ok": False, "msg": "slot 0-4"})
            return
        data = self.rfile.read(length)
        if len(data) != length:
            self._json(400, {"ok": False, "msg": "incomplete body"})
            return
        display_name = self.headers.get("X-Name", "").strip()
        if display_name and (len(display_name) > 32 or any(ord(ch) < 32 for ch in display_name)):
            self._json(400, {"ok": False, "msg": "bad name"})
            return

        try:
            from mywork_hub import voiceprint_np as vp
            from mywork_hub.asr import to_wav16k
            with tempfile.TemporaryDirectory(prefix="voiceprint-enroll-") as temp_dir:
                path = Path(temp_dir) / "source.wav"
                wav16 = Path(temp_dir) / "audio16k.wav"
                path.write_bytes(data)
                try:
                    to_wav16k(path, wav16)
                except Exception as conv_err:
                    log(f"[enroll] 音频转码失败: {conv_err}")
                    self._json(400, {"ok": False, "msg": f"音频转码失败: {str(conv_err)[:100]}"})
                    return
                if not wav16.exists() or wav16.stat().st_size < 4000:
                    self._json(400, {"ok": False, "msg": "转码后音频为空，请检查录音是否正常"})
                    return
                lib_path = cfg.root.parent / "voiceprints" / "library.json"
                res = vp.enroll(wav16, slot, lib_path, name=display_name or None)
                log(f"[enroll] slot={slot} bytes={len(data)} templates={res.get('samples', 0)} -> {res.get('ok')}")
                self._json(200 if res.get("ok") else 400, res)
        except Exception as exc:  # noqa: BLE001
            log(f"[enroll] 失败：{exc}")
            self._json(500, {"ok": False, "msg": str(exc)[:200]})

    def _do_ask(self):
        ip = self.client_address[0]
        if not rate_ok(ip):
            self._json(429, {"ok": False, "msg": "too many requests"})
            return
        auth = self.headers.get("Authorization", "")
        token = auth.removeprefix("Bearer ").strip()
        if not DEVICE_TOKEN or not hmac.compare_digest(token, DEVICE_TOKEN):
            self._json(401, {"ok": False, "msg": "unauthorized"})
            return
        length = int(self.headers.get("Content-Length", 0))
        if length <= 0 or length > MAX_BYTES:
            self._json(400, {"ok": False, "msg": "bad size"})
            return
        data = self.rfile.read(length)
        INBOX.mkdir(parents=True, exist_ok=True)
        path = INBOX / f"ask_{datetime.now().strftime('%Y%m%d_%H%M%S')}.wav"
        path.write_bytes(data)
        log(f"[ask] 问题音频 {len(data)} 字节")
        try:
            from mywork_hub.asr import to_wav16k
            wav16 = path.with_suffix(".16k.wav")
            to_wav16k(path, wav16)
            segs = transcribe_file(wav16)
            question = "".join(s["text"] for s in segs).strip()
            if not question:
                # The device treats HTTP 200 as playable PCM. Use a client
                # error for no-speech so it never plays the JSON error body.
                self._json(422, {"ok": False, "msg": "没听清，请再说一遍"})
                return
            log(f"[ask] 问题：{question}")
            answer = askmod.answer_question(question)
            log(f"[ask] 回答：{answer}")
            audio = askmod.tts(answer)
            if not audio:
                raise RuntimeError("TTS 空结果")
            # 设备输出通道是 24k/2ch：TTS 的 16k 单声道转成同格式裸 PCM
            import subprocess as sp
            tts_wav = path.with_suffix(".tts.wav")
            raw = path.with_suffix(".24k.pcm")
            tts_wav.write_bytes(audio)
            sp.run(["ffmpeg", "-y", "-loglevel", "error",
                    "-i", str(tts_wav), "-ar", "16000", "-ac", "2",
                    "-f", "s16le", str(raw)], check=True, capture_output=True)
            audio = raw.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "audio/wav")
            self.send_header("Content-Length", str(len(audio)))
            self.end_headers()
            self.wfile.write(audio)
        except Exception as exc:  # noqa: BLE001
            log(f"[ask] 失败：{exc}")
            self._json(500, {"ok": False, "msg": str(exc)[:200]})

    def _do_rename(self):
        """把某名额的声纹名字改成中文（设备无键盘，改名经此同步到服务器）"""
        auth = self.headers.get("Authorization", "")
        token = auth.removeprefix("Bearer ").strip()
        if not DEVICE_TOKEN or not hmac.compare_digest(token, DEVICE_TOKEN):
            self._json(401, {"ok": False, "msg": "unauthorized"})
            return
        length = int(self.headers.get("Content-Length", 0))
        if length <= 0 or length > 1024:
            self._json(400, {"ok": False, "msg": "bad size"})
            return
        try:
            req = json.loads(self.rfile.read(length))
            slot = int(req.get("slot", -1))
            name = str(req.get("name", "")).strip()[:16]
        except Exception:
            self._json(400, {"ok": False, "msg": "bad json"})
            return
        if not (0 <= slot <= 4) or not name:
            self._json(400, {"ok": False, "msg": "bad args"})
            return
        try:
            from mywork_hub import voiceprint_np as vp
            lib_path = cfg.root.parent / "voiceprints" / "library.json"
            lib = vp.load_library(lib_path)
            old = [k for k, v in lib.items() if v.get("slot") == slot]
            emb = []
            if old:
                emb = lib[old[0]].get("embedding", [])
            for k in old:
                lib.pop(k, None)
            if emb:
                lib[name] = {"slot": slot, "embedding": emb, "samples": 1}
            vp.save_library(lib_path, lib)
            log(f"[rename] slot={slot} {old} -> {name}")
            self._json(200, {"ok": True, "name": name})
        except Exception as exc:  # noqa: BLE001
            log(f"[rename] 失败：{exc}")
            self._json(500, {"ok": False, "msg": str(exc)[:200]})

    def _do_unenroll(self):
        """删除某个名额的声纹（长按删除时调用）"""
        auth = self.headers.get("Authorization", "")
        token = auth.removeprefix("Bearer ").strip()
        if not DEVICE_TOKEN or not hmac.compare_digest(token, DEVICE_TOKEN):
            self._json(401, {"ok": False, "msg": "unauthorized"})
            return
        try:
            slot = int(self.headers.get("X-Slot", "0"))
        except ValueError:
            self._json(400, {"ok": False, "msg": "bad slot"})
            return
        if not (0 <= slot <= 4):
            self._json(400, {"ok": False, "msg": "slot 0-4"})
            return
        try:
            from mywork_hub import voiceprint_np as vp
            lib_path = cfg.root.parent / "voiceprints" / "library.json"
            res = vp.unenroll(slot, lib_path)
            log(f"[unenroll] slot={slot} removed={res.get('removed', [])}")
            self._json(200 if res.get("ok") else 400, res)
        except Exception as exc:  # noqa: BLE001
            log(f"[unenroll] 失败：{exc}")
            self._json(500, {"ok": False, "msg": str(exc)[:200]})

    def _do_upload(self):
        ip = self.client_address[0]
        if not rate_ok(ip):
            self._json(429, {"ok": False, "msg": "too many requests"})
            return
        if self.path != "/v1/upload":
            self._json(404, {"ok": False, "msg": "unknown path"})
            return

        auth = self.headers.get("Authorization", "")
        token = auth.removeprefix("Bearer ").strip()
        if not DEVICE_TOKEN or not hmac.compare_digest(token, DEVICE_TOKEN):
            log(f"拒绝未授权请求 from {ip}")
            self._json(401, {"ok": False, "msg": "unauthorized"})
            return

        length = int(self.headers.get("Content-Length", 0))
        if length <= 0:
            self._json(400, {"ok": False, "msg": "empty body"})
            return
        if length > MAX_BYTES:
            self._json(413, {"ok": False, "msg": f"too large ({length} > {MAX_BYTES})"})
            return

        data = self.rfile.read(length)
        if len(data) != length:
            self._json(400, {"ok": False, "msg": "incomplete body"})
            return
        if len(data) < 1000:
            self._json(400, {"ok": False, "msg": "too small"})
            return

        INBOX.mkdir(parents=True, exist_ok=True)
        segment_id = self.headers.get("X-Segment-Id", "").strip()
        if segment_id and not re.fullmatch(r"[A-Za-z0-9._-]{1,80}", segment_id):
            self._json(400, {"ok": False, "msg": "bad segment id"})
            return
        if not segment_id:
            segment_id = f"legacy_{datetime.now().strftime('%Y%m%d_%H%M%S_%f')}_{time.time_ns() & 0xfffffff:x}"
        path = INBOX / f"dev_{segment_id}.wav"
        digest = hashlib.sha256(data).digest()
        duplicate = False
        with INBOX_LOCK:
            if path.exists():
                existing = path.read_bytes()
                if len(existing) != len(data) or hashlib.sha256(existing).digest() != digest:
                    self._json(409, {"ok": False, "msg": "segment id conflict"})
                    return
                duplicate = True
            else:
                part = INBOX / f".{path.name}.{os.getpid()}.{threading.get_ident()}.part"
                with part.open("wb") as fh:
                    fh.write(data)
                    fh.flush()
                    os.fsync(fh.fileno())
                os.replace(part, path)
                dir_fd = os.open(INBOX, os.O_RDONLY)
                try:
                    os.fsync(dir_fd)
                finally:
                    os.close(dir_fd)
        log(f"{'重复确认' if duplicate else '收到'} {len(data)} 字节 from {ip} -> {path.name}")

        self._json(202, {"ok": True, "id": segment_id, "bytes": len(data), "duplicate": duplicate})
        if duplicate:
            return
        threading.Thread(target=process, args=(path,), daemon=True).start()


def ask_tangseng(question: str) -> str:
    """通过 openclaw agent 让唐僧回答问题。"""
    import subprocess as sp

    msg = (
        f"【小智语音问答】有人通过语音设备问了以下问题，请直接简洁回答（50 字以内，"
        f"用于语音播报）。如果需要更多上下文，请查看今天的语音纪要。\n\n"
        f"问题：{question}"
    )
    cmd = [
        "openclaw", "agent", "--agent", OPENCLAW_AGENT,
        "--message", msg, "--json"
    ]
    try:
        result = sp.run(cmd, capture_output=True, text=True, timeout=120)
        # 从 JSON 输出里提取回复文本
        for line in result.stdout.splitlines():
            line = line.strip()
            if line.startswith("{"):
                try:
                    data = json.loads(line)
                    # openclaw --json 输出的格式可能在 payloads 里
                    if "payloads" in data:
                        for p in data["payloads"]:
                            if p.get("text"):
                                return p["text"]
                    if "text" in data:
                        return data["text"]
                except json.JSONDecodeError:
                    pass
        # JSON 解析失败则用原始输出
        text = result.stdout.strip()
        if text and len(text) > 10:
            return text[:200]
        return ""
    except Exception as exc:
        log(f"[ask] 唐僧调用失败: {exc}")
        return ""


def process(path: Path) -> None:
    with PROCESS_LOCK:
        try:
            log(f"开始处理 {path.name}")
            meta = pipeline.ingest(path, keep_audio=False)
            date = meta["date"]
            sessions = pipeline.load_day(date)
            daily = report.write_daily(date, sessions)
            frag = push.write_memory_fragment(date, daily)
            log(f"日报已更新：{daily.get('one_liner')}（片段 {frag.name}）")

            # 推送给唐僧
            result = push.push(date, daily)
            push.save_push_result(date, result)
            if result.get("sent"):
                log("已推送给唐僧")
            else:
                log(f"推送失败：{result.get('error') or result.get('stderr')}")
        except Exception as exc:  # noqa: BLE001
            log(f"处理失败：{exc}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--root", default=None, help="Mywork 根目录")
    args = ap.parse_args()
    if args.root:
        cfg.root = Path(args.root).expanduser().resolve()

    if not DEVICE_TOKEN:
        print("错误：必须设置 DEVICE_TOKEN 环境变量", file=sys.stderr)
        return 2

    log(f"监听 0.0.0.0:{args.port}  root={cfg.root}  agent={OPENCLAW_AGENT}")
    ThreadingHTTPServer(("0.0.0.0", args.port), Handler).serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
