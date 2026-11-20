"""本地 ASR：whisper.cpp（whisper-cli）封装。"""
from __future__ import annotations

import json
import subprocess
import tempfile
from pathlib import Path

from .config import cfg


class ASRError(RuntimeError):
    pass


def to_wav16k(src: Path, dst: Path) -> Path:
    """统一转成 16k 单声道 wav —— ASR 的标准输入。"""
    dst.parent.mkdir(parents=True, exist_ok=True)
    proc = subprocess.run(
        [
            "ffmpeg", "-y", "-loglevel", "error",
            "-i", str(src),
            "-ar", "16000", "-ac", "1", "-c:a", "pcm_s16le",
            str(dst),
        ],
        capture_output=True,
    )
    if proc.returncode != 0:
        raise ASRError(f"ffmpeg 转换失败: {proc.stderr.decode()[:300]}")
    return dst


def transcribe(wav: Path, language: str = "zh") -> list[dict]:
    """返回 [{"i", "start_ms", "end_ms", "text"}]"""
    if not Path(cfg.whisper_model).exists():
        raise ASRError(f"找不到模型 {cfg.whisper_model}，请先下载 ggml 模型")

    out_prefix = Path(tempfile.mkdtemp()) / "asr"
    cmd = [
        cfg.whisper_bin,
        "-m", cfg.whisper_model,
        "-f", str(wav),
        "-l", language,
        "-oj",
        "-of", str(out_prefix),
        "-np",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if proc.returncode != 0:
        raise ASRError(f"whisper 失败: {(proc.stderr or proc.stdout)[-500:]}")

    data = json.loads(out_prefix.with_suffix(".json").read_text(encoding="utf-8"))
    segs: list[dict] = []
    for item in data.get("transcription", []):
        text = (item.get("text") or "").strip()
        if not text:
            continue
        off = item.get("offsets") or {}
        start, end = int(off.get("from", 0)), int(off.get("to", 0))
        pieces = _split_sentences(text)
        span = max(end - start, 1)
        total = max(len(text), 1)
        acc = 0
        for piece in pieces:
            p_start = start + int(span * acc / total)
            acc += len(piece)
            p_end = start + int(span * acc / total)
            segs.append(
                {"i": len(segs), "start_ms": p_start, "end_ms": max(p_end, p_start + 300), "text": piece}
            )
    return segs


def _split_sentences(text: str) -> list[str]:
    """按中英文句末标点切句，过短的句子并到上一句。"""
    import re

    parts = re.split(r"(?<=[。！？?!])", text)
    out: list[str] = []
    for p in parts:
        p = p.strip()
        if not p:
            continue
        if out and (len(p) < 8 or not p.endswith(("。", "！", "？", "!", "?"))):
            out[-1] += p
        else:
            out.append(p)
    return out or [text]
