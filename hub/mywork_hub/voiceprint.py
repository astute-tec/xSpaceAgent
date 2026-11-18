"""声纹录入与识别：resemblyzer 生成 256 维声纹向量，余弦相似度匹配。"""
from __future__ import annotations

import json
import struct
from pathlib import Path

import numpy as np

from resemblyzer import VoiceEncoder, preprocess_wav

from .config import cfg

ENCODER = None
VOICES_DIR = Path(__file__).resolve().parents[2] / "voiceprints"


def _encoder():
    global ENCODER
    if ENCODER is None:
        ENCODER = VoiceEncoder()
    return ENCODER


def load_library() -> dict:
    """加载声纹库 {name: embedding_list}"""
    f = VOICES_DIR / "library.json"
    if f.exists():
        return json.loads(f.read_text(encoding="utf-8"))
    return {}


def save_library(lib: dict) -> None:
    VOICES_DIR.mkdir(parents=True, exist_ok=True)
    f = VOICES_DIR / "library.json"
    f.write_text(json.dumps(lib, ensure_ascii=False, indent=2), encoding="utf-8")


def enroll(name: str, wav_paths: list[str]) -> dict:
    """从多段录音中提取声纹，注册到声纹库。"""
    enc = _encoder()
    embeddings = []
    for wp in wav_paths:
        wav = preprocess_wav(Path(wp))
        if len(wav) < 16000:  # 至少 1 秒
            continue
        emb = enc.embed_utterance(wav)
        embeddings.append(emb)
    if not embeddings:
        return {"ok": False, "msg": "没有有效的录音段"}

    # 取平均作为该人的声纹模板
    avg = np.mean(embeddings, axis=0)
    avg = avg / np.linalg.norm(avg)

    lib = load_library()
    lib[name] = {
        "embedding": avg.tolist(),
        "samples": len(embeddings),
    }
    save_library(lib)
    return {"ok": True, "name": name, "samples": len(embeddings), "dim": len(avg)}


def identify(wav_path: str, threshold: float = 0.65) -> dict:
    """识别一段音频的说话人，返回最匹配的人和置信度。"""
    lib = load_library()
    if not lib:
        return {"speaker": "其他人", "confidence": 0.0}

    enc = _encoder()
    wav = preprocess_wav(Path(wav_path))
    if len(wav) < 16000:
        return {"speaker": "其他人", "confidence": 0.0}
    emb = enc.embed_utterance(wav)
    emb = emb / np.linalg.norm(emb)

    best_name, best_score = None, -1.0
    for name, info in lib.items():
        ref = np.array(info["embedding"])
        score = float(np.dot(emb, ref))
        if score > best_score:
            best_score = score
            best_name = name

    if best_score >= threshold:
        return {"speaker": best_name, "confidence": round(best_score, 3)}
    return {"speaker": "其他人", "confidence": round(best_score, 3)}


def list_speakers() -> list[str]:
    return list(load_library().keys())
