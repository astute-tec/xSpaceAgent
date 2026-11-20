"""纯 numpy 声纹识别。

面向固定 5 人的小规模闭集场景：录入时从一段语音提取多个 MFCC 模板，
识别时逐 ASR 段比对。低置信结果一律归“其他人”。
"""
from __future__ import annotations

import json
import os
import threading
import wave
from contextlib import contextmanager
from datetime import datetime
from pathlib import Path

import numpy as np

try:
    import fcntl
except ImportError:  # pragma: no cover - Hub 部署在 Linux/macOS
    fcntl = None

SR = 16000
N_MFCC = 20
N_MEL = 26
FRAME = 400
HOP = 160
PRE_EMPH = 0.97
WINDOW_SECONDS = 2.0
WINDOW_STEP_SECONDS = 1.0
MAX_TEMPLATES = 6
LOW_THRESHOLD = 0.55
HIGH_THRESHOLD = 0.72

_LIB_LOCK = threading.RLock()


def read_wav(path: str | Path) -> np.ndarray:
    """读 WAV，返回 16k 单声道 float32 [-1, 1]。"""
    with wave.open(str(path), "rb") as w:
        ch = w.getnchannels()
        sw = w.getsampwidth()
        sr = w.getframerate()
        raw = w.readframes(w.getnframes())
    if sw == 2:
        x = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    elif sw == 4:
        x = np.frombuffer(raw, dtype="<i4").astype(np.float32) / 2147483648.0
    elif sw == 1:
        x = (np.frombuffer(raw, dtype=np.uint8).astype(np.float32) - 128) / 128.0
    else:
        raise ValueError(f"不支持的 WAV 位深: {sw * 8}")
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    if sr != SR and len(x):
        n = max(1, int(len(x) * SR / sr))
        idx = np.linspace(0, len(x) - 1, n)
        x = np.interp(idx, np.arange(len(x)), x).astype(np.float32)
    return x


def _mel_filterbank() -> np.ndarray:
    def hz2mel(f):
        return 2595.0 * np.log10(1.0 + f / 700.0)

    def mel2hz(m):
        return 700.0 * (10.0 ** (m / 2595.0) - 1.0)

    lo, hi = hz2mel(80), hz2mel(7600)
    mels = np.linspace(lo, hi, N_MEL + 2)
    freqs = mel2hz(mels)
    bins = np.floor((FRAME + 1) * freqs / SR).astype(int)
    fb = np.zeros((N_MEL, FRAME // 2 + 1), dtype=np.float32)
    for m in range(1, N_MEL + 1):
        left, center, right = bins[m - 1], bins[m], bins[m + 1]
        for k in range(left, center):
            fb[m - 1, k] = (k - left) / max(center - left, 1)
        for k in range(center, right):
            fb[m - 1, k] = (right - k) / max(right - center, 1)
    return fb


_FB = _mel_filterbank()
_WIN = np.hamming(FRAME).astype(np.float32)


def _dct_matrix() -> np.ndarray:
    n = N_MEL
    k = np.arange(N_MFCC).reshape(-1, 1)
    i = np.arange(n).reshape(1, -1)
    mat = np.cos(np.pi * k * (2 * i + 1) / (2 * n)) * np.sqrt(2.0 / n)
    mat[0] *= 1 / np.sqrt(2)
    return mat.astype(np.float32)


_DCT = _dct_matrix()


def mfcc(x: np.ndarray) -> np.ndarray:
    """返回 (帧数, N_MFCC)。"""
    if len(x) < FRAME:
        return np.zeros((1, N_MFCC), dtype=np.float32)
    x = np.append(x[0], x[1:] - PRE_EMPH * x[:-1])
    n = 1 + (len(x) - FRAME) // HOP
    frames = np.lib.stride_tricks.sliding_window_view(x, FRAME)[::HOP][:n]
    spec = np.abs(np.fft.rfft(frames * _WIN, axis=1)) ** 2
    mel = np.log(np.maximum(spec @ _FB.T, 1e-10))
    return mel @ _DCT.T


def _frame_rms(x: np.ndarray, frame: int = 320, hop: int = 160) -> np.ndarray:
    if len(x) < frame:
        return np.array([float(np.sqrt(np.mean(np.square(x)))) if len(x) else 0.0])
    frames = np.lib.stride_tricks.sliding_window_view(x, frame)[::hop]
    return np.sqrt(np.mean(np.square(frames), axis=1))


def analyze_signal(x: np.ndarray) -> dict:
    """返回录入质量指标和准入结论。"""
    duration = len(x) / SR
    rms = float(np.sqrt(np.mean(np.square(x)))) if len(x) else 0.0
    peak = float(np.max(np.abs(x))) if len(x) else 0.0
    clipping = float(np.mean(np.abs(x) >= 0.985)) if len(x) else 0.0
    frms = _frame_rms(x)
    floor = float(np.percentile(frms, 20)) if len(frms) else 0.0
    active_threshold = max(0.006, min(floor * 2.2, max(rms * 0.75, 0.006)))
    active_ratio = float(np.mean(frms >= active_threshold)) if len(frms) else 0.0

    reasons = []
    if duration < 2.5:
        reasons.append("录音少于2.5秒")
    if rms < 0.0045 or peak < 0.025:
        reasons.append("声音太小或接近静音")
    if active_ratio < 0.12:
        reasons.append("有效语音太少")
    if clipping > 0.03:
        reasons.append("削波过多，请离设备远一点")
    return {
        "ok": not reasons,
        "duration_s": round(duration, 3),
        "rms": round(rms, 6),
        "peak": round(peak, 6),
        "active_ratio": round(active_ratio, 4),
        "clipping_ratio": round(clipping, 6),
        "reason": "；".join(reasons),
    }


def embed_signal(x: np.ndarray) -> np.ndarray | None:
    """从一段 PCM 提取归一化的 MFCC 均值+标准差向量。"""
    if len(x) < SR:
        return None
    m = mfcc(x)
    frms = _frame_rms(x, FRAME, HOP)[: len(m)]
    if len(frms):
        threshold = max(0.004, float(np.percentile(frms, 25)) * 1.35)
        keep = m[frms >= threshold]
    else:
        keep = m
    if len(keep) < 10:
        keep = m
    v = np.concatenate([keep.mean(axis=0), keep.std(axis=0)])
    norm = np.linalg.norm(v)
    return (v / norm).astype(np.float32) if norm > 0 else None


def embed(path: str | Path) -> np.ndarray | None:
    return embed_signal(read_wav(path))


def _window_embeddings(x: np.ndarray) -> list[np.ndarray]:
    window = int(WINDOW_SECONDS * SR)
    step = int(WINDOW_STEP_SECONDS * SR)
    if len(x) < window:
        vec = embed_signal(x)
        return [vec] if vec is not None else []

    candidates: list[tuple[float, np.ndarray]] = []
    for start in range(0, len(x) - window + 1, step):
        part = x[start:start + window]
        quality = analyze_signal(part)
        if quality["rms"] < 0.004 or quality["clipping_ratio"] > 0.05:
            continue
        vec = embed_signal(part)
        if vec is not None:
            score = quality["active_ratio"] * 2.0 + quality["rms"]
            candidates.append((score, vec))
    candidates.sort(key=lambda item: item[0], reverse=True)
    return [vec for _, vec in candidates[:MAX_TEMPLATES]]


def cosine(a, b) -> float:
    a = np.asarray(a, dtype=np.float32)
    b = np.asarray(b, dtype=np.float32)
    if a.shape != b.shape or not a.size:
        return 0.0
    na, nb = np.linalg.norm(a), np.linalg.norm(b)
    if na == 0 or nb == 0:
        return 0.0
    return float(np.dot(a, b) / (na * nb))


def _prepare_private_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)
    try:
        os.chmod(path, 0o700)
    except OSError:
        pass


@contextmanager
def _library_guard(path: Path):
    _prepare_private_dir(path.parent)
    if path.exists():
        try:
            os.chmod(path, 0o600)
        except OSError:
            pass
    lock_path = path.with_suffix(path.suffix + ".lock")
    with _LIB_LOCK:
        fd = os.open(lock_path, os.O_CREAT | os.O_RDWR, 0o600)
        try:
            os.chmod(lock_path, 0o600)
            if fcntl is not None:
                fcntl.flock(fd, fcntl.LOCK_EX)
            yield
        finally:
            if fcntl is not None:
                fcntl.flock(fd, fcntl.LOCK_UN)
            os.close(fd)


def _read_library_unlocked(path: Path) -> dict:
    if not path.exists():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def _write_library_unlocked(path: Path, lib: dict) -> None:
    payload = json.dumps(lib, ensure_ascii=False, indent=2) + "\n"
    temp = path.with_name(f".{path.name}.{os.getpid()}.{threading.get_ident()}.tmp")
    fd = os.open(temp, os.O_CREAT | os.O_TRUNC | os.O_WRONLY, 0o600)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", closefd=False) as fh:
            fh.write(payload)
            fh.flush()
            os.fsync(fh.fileno())
        os.close(fd)
        fd = -1
        os.replace(temp, path)
        os.chmod(path, 0o600)
        if os.name == "posix":       # 目录 fsync 仅 POSIX 支持（Windows 打开目录会 PermissionError）
            dir_fd = os.open(path.parent, os.O_RDONLY)
            try:
                os.fsync(dir_fd)
            finally:
                os.close(dir_fd)
    finally:
        if fd >= 0:
            os.close(fd)
        temp.unlink(missing_ok=True)


def load_library(path: str | Path) -> dict:
    p = Path(path)
    with _library_guard(p):
        return _read_library_unlocked(p)


def save_library(path: str | Path, lib: dict) -> None:
    p = Path(path)
    with _library_guard(p):
        _write_library_unlocked(p, lib)


def enroll(path: str | Path, slot: int, lib_path: str | Path, name: str | None = None) -> dict:
    """登记 slot 0-4。成功后只保存 embedding，不保存原始音频。"""
    if not 0 <= slot <= 4:
        return {"ok": False, "msg": "slot 必须是 0-4"}
    x = read_wav(path)
    quality = analyze_signal(x)
    if not quality["ok"]:
        return {"ok": False, "msg": quality["reason"], "quality": quality}
    vectors = _window_embeddings(x)
    if not vectors:
        return {"ok": False, "msg": "未提取到有效声纹模板", "quality": quality}

    center = np.mean(np.stack(vectors), axis=0)
    center /= max(float(np.linalg.norm(center)), 1e-8)
    display_name = (name or f"成员{slot + 1}").strip()[:32] or f"成员{slot + 1}"
    entry = {
        "schema_version": 2,
        "slot": slot,
        "name": display_name,
        "embedding": [round(float(v), 6) for v in center],
        "embeddings": [[round(float(v), 6) for v in vec] for vec in vectors],
        "samples": len(vectors),
        "quality": quality,
        "updated_at": datetime.now().isoformat(timespec="seconds"),
    }

    p = Path(lib_path)
    with _library_guard(p):
        lib = _read_library_unlocked(p)
        for key in [key for key, info in lib.items() if info.get("slot") == slot]:
            lib.pop(key, None)
        key = display_name
        if key in lib and lib[key].get("slot") != slot:
            key = f"{display_name}_{slot + 1}"
        lib[key] = entry
        _write_library_unlocked(p, lib)
    return {
        "ok": True,
        "name": display_name,
        "slot": slot,
        "samples": len(vectors),
        "quality": quality,
    }


def rename(slot: int, name: str, lib_path: str | Path) -> dict:
    """Change the display name atomically without rebuilding trained templates."""
    display_name = name.strip()[:32]
    if not 0 <= slot <= 4 or not display_name:
        return {"ok": False, "msg": "无效的名额或姓名"}
    p = Path(lib_path)
    with _library_guard(p):
        lib = _read_library_unlocked(p)
        keys = [key for key, info in lib.items() if info.get("slot") == slot]
        if not keys:
            return {"ok": False, "msg": "该名额尚未录入"}
        entry = dict(lib[keys[0]])
        entry["name"] = display_name
        for key in keys:
            lib.pop(key)
        key = display_name
        if key in lib and lib[key].get("slot") != slot:
            key = f"{display_name}_{slot + 1}"
        lib[key] = entry
        _write_library_unlocked(p, lib)
    return {"ok": True, "slot": slot, "name": display_name}


def unenroll(slot: int, lib_path: str | Path) -> dict:
    if not 0 <= slot <= 4:
        return {"ok": False, "msg": "slot 必须是 0-4"}
    p = Path(lib_path)
    with _library_guard(p):
        lib = _read_library_unlocked(p)
        removed = [key for key, info in lib.items() if info.get("slot") == slot]
        for key in removed:
            lib.pop(key, None)
        _write_library_unlocked(p, lib)
    return {"ok": True, "removed": removed, "slot": slot}


def _entry_score(vec: np.ndarray, info: dict) -> float:
    center_score = cosine(vec, info.get("embedding", []))
    templates = info.get("embeddings") or []
    template_score = max((cosine(vec, item) for item in templates), default=center_score)
    return 0.65 * center_score + 0.35 * template_score


def _best_match(vec: np.ndarray, lib: dict) -> tuple[str | None, float]:
    best_name, best_score = None, -1.0
    for key, info in lib.items():
        score = _entry_score(vec, info)
        if score > best_score:
            best_name = info.get("name") or key
            best_score = score
    return best_name, best_score


def identify_signal(x: np.ndarray, lib: dict, threshold: float = HIGH_THRESHOLD) -> dict:
    if not lib:
        return {"speaker": "其他人", "confidence": 0.0}
    vec = embed_signal(x)
    if vec is None:
        return {"speaker": "其他人", "confidence": 0.0}
    best, score = _best_match(vec, lib)
    if best is not None and score >= threshold:
        return {"speaker": best, "confidence": round(score, 3)}
    return {"speaker": "其他人", "confidence": round(max(score, 0.0), 3)}


def identify(path: str | Path, lib_path: str | Path, threshold: float = HIGH_THRESHOLD) -> dict:
    return identify_signal(read_wav(path), load_library(lib_path), threshold)


def identify_segments(
    path: str | Path,
    segments: list[dict],
    lib_path: str | Path,
    low_threshold: float = LOW_THRESHOLD,
    high_threshold: float = HIGH_THRESHOLD,
) -> list[dict]:
    """逐 ASR 段识别，并对中置信结果使用前后 10 秒高置信投票。"""
    lib = load_library(lib_path)
    if not lib:
        return [{"speaker": "其他人", "confidence": 0.0} for _ in segments]
    audio = read_wav(path)
    duration_ms = len(audio) * 1000 / SR
    raw_results = []

    for row in segments:
        start_ms = max(0.0, float(row.get("start_ms", 0)))
        end_ms = min(duration_ms, float(row.get("end_ms", start_ms)))
        if end_ms <= start_ms:
            raw_results.append({"speaker": "其他人", "confidence": 0.0, "candidate": None})
            continue
        center = (start_ms + end_ms) / 2.0
        window_ms = min(3000.0, max(1500.0, end_ms - start_ms))
        left = max(0.0, center - window_ms / 2.0)
        right = min(duration_ms, left + window_ms)
        left = max(0.0, right - window_ms)
        part = audio[int(left * SR / 1000):int(right * SR / 1000)]
        vec = embed_signal(part)
        if vec is None:
            raw_results.append({"speaker": "其他人", "confidence": 0.0, "candidate": None})
            continue
        candidate, score = _best_match(vec, lib)
        speaker = candidate if candidate is not None and score >= high_threshold else "其他人"
        raw_results.append({
            "speaker": speaker,
            "confidence": round(max(score, 0.0), 3),
            "candidate": candidate if score >= low_threshold else None,
            "high": score >= high_threshold,
            "center_ms": center,
        })

    results = []
    for index, item in enumerate(raw_results):
        speaker = item["speaker"]
        source = "direct" if item.get("high") else "low"
        if speaker == "其他人" and item.get("candidate"):
            votes: dict[str, int] = {}
            for neighbor in raw_results:
                if not neighbor.get("high"):
                    continue
                if abs(neighbor.get("center_ms", 0) - item.get("center_ms", 0)) > 10000:
                    continue
                name = neighbor["speaker"]
                votes[name] = votes.get(name, 0) + 1
            if votes:
                winner, count = max(votes.items(), key=lambda pair: pair[1])
                tied = sum(1 for value in votes.values() if value == count) > 1
                if not tied and winner == item["candidate"]:
                    speaker = winner
                    source = "context"
        results.append({
            "speaker": speaker,
            "confidence": item["confidence"],
            "source": source,
            "segment_index": index,
        })
    return results
