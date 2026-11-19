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
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mywork_hub import pipeline, push, report  # noqa: E402
from mywork_hub import ask as askmod  # noqa: E402
from mywork_hub.asr_aliyun import transcribe_file  # noqa: E402
from mywork_hub.config import cfg  # noqa: E402
from mywork_hub.inbox import DurableInbox, SegmentConflict, processed_audio_names  # noqa: E402

MAX_BYTES = int(os.environ.get("MAX_UPLOAD_MB", "32")) * 1024 * 1024
DEVICE_TOKEN = os.environ.get("DEVICE_TOKEN", "")
OPENCLAW_AGENT = os.environ.get("OPENCLAW_AGENT", "tangseng")
INBOX = Path(os.environ.get("INBOX_DIR", Path.home() / "myWork" / "05_语音工作日志" / "_inbox"))
DEPLOY_DIR = Path(os.environ.get("DEPLOY_DIR", Path(__file__).resolve().parents[2] / "deploy" / "pkg"))
PROCESS_LOCK = threading.Lock()
INBOX_LOCK = threading.Lock()
RATE: dict[str, list[float]] = {}
ASK_JOBS: dict[str, dict] = {}
ASK_JOBS_LOCK = threading.Lock()
ASK_AUDIO_CONDITION = threading.Condition(ASK_JOBS_LOCK)
RECORDING_WAKE = threading.Event()
_recording_inboxes: dict[str, DurableInbox] = {}


def recording_inbox() -> DurableInbox:
    key = str(INBOX.resolve())
    with INBOX_LOCK:
        if key not in _recording_inboxes:
            _recording_inboxes[key] = DurableInbox(INBOX)
        return _recording_inboxes[key]

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

    def _read_body(self, length: int) -> bytes:
        self.connection.settimeout(20)
        data = bytearray()
        started = time.monotonic()
        try:
            while len(data) < length:
                part = self.rfile.read1(min(65536, length - len(data)))
                if not part: break
                data.extend(part)
        except TimeoutError:
            self.close_connection = True
            log(f"[http] {self.path} body timeout {len(data)}/{length}")
        if len(data) != length or time.monotonic() - started > 3:
            log(f"[http] {self.path} body={len(data)}/{length} elapsed={time.monotonic()-started:.1f}s")
        return bytes(data)

    def _json(self, code: int, obj: dict) -> None:
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/v1/voiceprints":
            token = self.headers.get("Authorization", "").removeprefix("Bearer ").strip()
            if not DEVICE_TOKEN or not hmac.compare_digest(token, DEVICE_TOKEN):
                self._json(401, {"ok": False})
                return
            from mywork_hub import voiceprint_np as vp
            lib = vp.load_library(cfg.root.parent / "voiceprints" / "library.json")
            slots = [{"slot": entry["slot"], "name": entry["name"]}
                     for entry in lib.values() if "slot" in entry and "name" in entry]
            self._json(200, {"ok": True, "slots": slots})
            return
        if self.path.startswith("/v1/ask/"):
            token = self.headers.get("Authorization", "").removeprefix("Bearer ").strip()
            if not DEVICE_TOKEN or not hmac.compare_digest(token, DEVICE_TOKEN):
                self._json(401, {"ok": False})
                return
            job_id = self.path.removeprefix("/v1/ask/")
            streaming = job_id.endswith("/audio")
            if streaming:
                job_id = job_id.removesuffix("/audio")
                self._stream_answer(job_id)
                return
            with ASK_JOBS_LOCK:
                job = dict(ASK_JOBS.get(job_id) or {})
            if not job:
                self._json(404, {"ok": False, "msg": "job expired"})
            elif self.headers.get("X-Ask-Status") == "1":
                # Publish text before TTS/PCM transfer, even if TTS subsequently fails.
                self._json(202, {"ok": True, "job": job_id, "stage": job["stage"],
                                 "state": job["state"], "answer": job.get("answer", ""),
                                 "stream_ready": job.get("stream_ready", False),
                                 "msg": job.get("error", "")})
            elif job["state"] == "done":
                audio = job["audio"]
                self.send_response(200)
                self.send_header("Content-Type", "audio/pcm;rate=16000;channels=2;format=s16le")
                self.send_header("Content-Length", str(len(audio)))
                self.end_headers()
                self.wfile.write(audio)
                log(f"[ask {job_id}] 播报音频已返回 {len(audio)} 字节")
            elif job["state"] == "failed":
                self._json(422, {"ok": False, "stage": job["stage"], "msg": job["error"]})
            else:
                self._json(202, {"ok": True, "job": job_id, "stage": job["stage"]})
            return
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
        data = self._read_body(length)
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
        data = self._read_body(length)
        if len(data) != length:
            log(f"[ask] 请求体不完整 {len(data)}/{length}")
            self._json(400, {"ok": False, "msg": "incomplete body"})
            return
        if self.headers.get("X-Async-Response") == "1":
            with ASK_JOBS_LOCK:
                now = time.monotonic()
                expired = [key for key, item in ASK_JOBS.items() if now - item["created"] > 600]
                for key in expired: ASK_JOBS.pop(key, None)
                if len(ASK_JOBS) >= 4:
                    completed = [key for key, item in ASK_JOBS.items() if item["state"] != "working"]
                    if completed: ASK_JOBS.pop(completed[0], None)
                if len(ASK_JOBS) >= 4:
                    self._json(503, {"ok": False, "msg": "ask queue busy"})
                    return
                job_id = uuid.uuid4().hex
                ASK_JOBS[job_id] = {"state": "working", "stage": "received", "created": now,
                                    "streaming": self.headers.get("X-Stream-Audio") == "1"}
            threading.Thread(target=process_ask, args=(job_id, data), daemon=True).start()
            log(f"[ask {job_id}] 问题音频已完整接收 {len(data)} 字节")
            self._json(202, {"ok": True, "job": job_id, "stage": "received"})
            return
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
            result = vp.rename(slot, name, lib_path)
            log(f"[rename] slot={slot} name={name} ok={result['ok']}")
            self._json(200 if result["ok"] else 404, result)
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

        data = self._read_body(length)
        if len(data) != length:
            self._json(400, {"ok": False, "msg": "incomplete body"})
            return
        if len(data) < 80:
            self._json(400, {"ok": False, "msg": "too small"})
            return

        INBOX.mkdir(parents=True, exist_ok=True)
        segment_id = self.headers.get("X-Segment-Id", "").strip()
        if segment_id and not re.fullmatch(r"[A-Za-z0-9._-]{1,80}", segment_id):
            self._json(400, {"ok": False, "msg": "bad segment id"})
            return
        if not segment_id:
            segment_id = f"legacy_{datetime.now().strftime('%Y%m%d_%H%M%S_%f')}_{time.time_ns() & 0xfffffff:x}"
        digest = hashlib.sha256(data).hexdigest()
        expected = self.headers.get("X-Content-SHA256", "").strip().lower()
        if expected and not hmac.compare_digest(expected, digest):
            self._json(400, {"ok": False, "msg": "sha256 mismatch"})
            return
        suffix = ".opus" if data.startswith(b"OggS") else ".wav"
        if suffix == ".opus":
            valid = b"OpusHead" in data[:64]
        else:
            valid = data[:4] == b"RIFF" and data[8:12] == b"WAVE"
        if not valid:
            self._json(415, {"ok": False, "msg": "expected WAV or Ogg Opus"})
            return
        captured = self.headers.get("X-Captured-At", "")
        if captured and (not captured.isdecimal() or not 1700000000 <= int(captured) <= time.time()+300):
            self._json(400, {"ok": False, "msg": "bad capture timestamp"})
            return
        try:
            receipt = recording_inbox().receive(segment_id, data, suffix, int(captured) if captured else None)
        except SegmentConflict:
            self._json(409, {"ok": False, "msg": "segment id conflict"})
            return
        except (OSError, sqlite3.Error) as exc:
            log(f"保存录音失败：{type(exc).__name__}")
            self._json(503, {"ok": False, "msg": "durable storage unavailable"})
            return
        log(f"录音已持久接收 id={segment_id} bytes={len(data)} duplicate={receipt['duplicate']}")
        RECORDING_WAKE.set()
        self._json(202, receipt)

    def _stream_answer(self, job_id: str):
        """Bounded producer wait; preserve legacy full-audio GET for old firmware."""
        deadline = time.monotonic()+90
        with ASK_AUDIO_CONDITION:
            job = ASK_JOBS.get(job_id)
            if not job or not job.get("streaming"):
                self._json(404, {"ok": False, "msg": "stream unavailable"})
                return
            while not job.get("chunks") and job["state"] == "working" and time.monotonic()<deadline:
                ASK_AUDIO_CONDITION.wait(min(1, max(0, deadline-time.monotonic())))
            if not job.get("chunks"):
                self._json(422, {"ok": False, "msg": job.get("error", "audio timeout")})
                return
        self.send_response(200)
        self.send_header("Content-Type", "audio/pcm;rate=16000;channels=1;format=s16le")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        index=0
        try:
            while time.monotonic()<deadline:
                with ASK_AUDIO_CONDITION:
                    chunks=job.get("chunks", [])
                    if index<len(chunks):
                        chunk=chunks[index]; index+=1
                    elif job["state"] == "done":
                        self.wfile.write(b"0\r\n\r\n"); self.wfile.flush(); return
                    elif job["state"] == "failed":
                        return  # incomplete chunked body lets the device detect failure
                    else:
                        ASK_AUDIO_CONDITION.wait(1); continue
                self.wfile.write(f"{len(chunk):x}\r\n".encode()+chunk+b"\r\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            log(f"[ask {job_id}] 播放连接断开，已保留文字结果")


def process_ask(job_id: str, data: bytes) -> None:
    started = time.monotonic()

    def progress(stage: str):
        with ASK_JOBS_LOCK:
            ASK_JOBS[job_id]["stage"] = stage
        log(f"[ask {job_id}] stage={stage} elapsed={time.monotonic()-started:.1f}s")

    try:
        from mywork_hub.asr import to_wav16k
        with tempfile.TemporaryDirectory(prefix="voice-ask-") as directory:
            path = Path(directory) / "question.wav"
            wav16 = Path(directory) / "question16.wav"
            path.write_bytes(data)
            progress("transcribing")
            to_wav16k(path, wav16)
            question = "".join(item["text"] for item in transcribe_file(wav16)).strip()
            if not question:
                raise ValueError("没听清，请靠近后重新说")
            log(f"[ask {job_id}] ASR文字：{question}")
            answer = askmod.answer_question(question, progress=progress)
            if not answer.strip():
                raise ValueError("没有收到回答")
            log(f"[ask {job_id}] 回答：{answer}")
            with ASK_JOBS_LOCK:
                ASK_JOBS[job_id]["answer"] = answer if len(answer) <= 384 else answer[:383] + "…"
            progress("tts")
            with ASK_JOBS_LOCK:
                streaming = ASK_JOBS[job_id].get("streaming", False)
            if streaming:
                with ASK_AUDIO_CONDITION:
                    ASK_JOBS[job_id].update(stream_ready=True, chunks=[])
                    ASK_AUDIO_CONDITION.notify_all()
                total = 0
                for chunk in askmod.iter_tts_pcm(answer):
                    total += len(chunk)
                    if total > 960000 or len(chunk) % 2:
                        raise ValueError("流式语音超过30秒或样本不完整")
                    with ASK_AUDIO_CONDITION:
                        ASK_JOBS[job_id]["chunks"].append(chunk)
                        ASK_AUDIO_CONDITION.notify_all()
                if not total:
                    raise ValueError("语音合成没有返回音频")
                with ASK_AUDIO_CONDITION:
                    mono = b"".join(ASK_JOBS[job_id]["chunks"])
                    # Retain the old stereo PCM GET contract during staged upgrades.
                    stereo = bytearray(total*2)
                    for i in range(0, total, 2):
                        stereo[i*2:i*2+4] = mono[i:i+2]*2
                    ASK_JOBS[job_id].update(state="done", stage="done", audio=bytes(stereo))
                    ASK_AUDIO_CONDITION.notify_all()
                log(f"[ask {job_id}] 流式单声道播报生成完毕 bytes={total} elapsed={time.monotonic()-started:.1f}s")
                return
            audio = askmod.tts(answer)
            if not audio:
                raise ValueError("语音合成没有返回音频")
            tts_path = Path(directory) / "answer.wav"
            tts_path.write_bytes(audio)
            result = subprocess.run(
                ["ffmpeg", "-loglevel", "error", "-i", str(tts_path),
                 "-ar", "16000", "-ac", "2", "-f", "s16le", "pipe:1"],
                capture_output=True, check=True, timeout=15,
            )
            pcm = result.stdout
            if not pcm or len(pcm) > 1920000 or len(pcm) % 4:
                raise ValueError("回答音频格式或长度超出设备限制")
        with ASK_JOBS_LOCK:
            ASK_JOBS[job_id].update(state="done", stage="done", audio=pcm)
        log(f"[ask {job_id}] 播报已生成 {len(pcm)} 字节 total={time.monotonic()-started:.1f}s")
    except Exception as exc:
        with ASK_AUDIO_CONDITION:
            ASK_JOBS[job_id].update(state="failed", error=str(exc)[:120])
            ASK_AUDIO_CONDITION.notify_all()
        log(f"[ask {job_id}] 失败：{type(exc).__name__}: {exc}")


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


def process(item: dict) -> None:
    with PROCESS_LOCK:
        try:
            path = Path(item["path"])
            log(f"开始处理 {path.name}")
            if hashlib.sha256(path.read_bytes()).hexdigest() != item["sha256"]:
                raise ValueError("stored audio checksum mismatch")
            # Old firmware has no trusted capture timestamp. Fix the fallback
            # archive date to receipt time so a retry across midnight cannot
            # create a second session on a different day.
            archive_date = item.get("date") or datetime.fromtimestamp(
                item.get("captured_at") or item["received_at"]).strftime("%Y-%m-%d")
            meta = pipeline.ingest(path, keep_audio=False, session_id=item["session_id"],
                                   date=archive_date, captured_at=item.get("captured_at"))
            date = meta["date"]
            recording_inbox().complete(item, date)
            log(f"录音已归档 id={item['id']} date={date}，日报按窗口合并")
        except Exception as exc:  # noqa: BLE001
            recording_inbox().fail(item, str(exc))
            log(f"录音处理失败，已持久安排重试 id={item['id']} error={type(exc).__name__}")


def recording_worker():
    while True:
        try:
            with ASK_JOBS_LOCK:
                asking = any(job.get("state") == "working" for job in ASK_JOBS.values())
            if not asking:
                inbox = recording_inbox()
                # Check due reports even under a continuous recording backlog.
                due = inbox.due_report()
                if due:
                    try:
                        date=due["date"]
                        daily=report.write_daily(date,pipeline.load_day(date))
                        push.write_memory_fragment(date,daily)
                        result=push.push(date,daily)
                        push.save_push_result(date,result)
                        if not result.get("sent"):
                            raise RuntimeError("唐僧通知未确认")
                        inbox.finish_report(due)
                    except Exception as exc:
                        inbox.finish_report(due,str(exc))
                item=inbox.claim()
                if item:
                    process(item)
                    continue
        except Exception as exc:
            log(f"录音后台队列等待重试：{type(exc).__name__}")
        RECORDING_WAKE.wait(1)
        RECORDING_WAKE.clear()


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

    restored=recording_inbox().recover(processed_audio_names(cfg.work_dir))
    log(f"持久录音队列恢复 imported={restored} states={recording_inbox().stats()}")
    threading.Thread(target=recording_worker,name="recording-worker",daemon=True).start()
    log(f"监听 0.0.0.0:{args.port}  root={cfg.root}  agent={OPENCLAW_AGENT}")
    ThreadingHTTPServer(("0.0.0.0", args.port), Handler).serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
