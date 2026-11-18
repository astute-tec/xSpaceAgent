"""会话处理流水线：音频 → 转写 → 过滤 → 落盘。"""
from __future__ import annotations

import json
import shutil
from datetime import datetime
from pathlib import Path

from . import filter as flt
from . import store
from .asr import to_wav16k, transcribe
from .config import cfg as _cfg
from .config import cfg


def _speaker_position(src_wav: Path) -> str:
    """双麦左右声道能量比 → 说话人方位标记（L/R/M）。

    两个人坐在设备两侧时，各自的段会聚成两簇，配合内容即可映射姓名。
    """
    import subprocess as sp
    try:
        out = sp.run(
            ["ffprobe", "-v", "error", "-select_streams", "a:0",
             "-show_entries", "stream=channels", "-of", "csv=p=0", str(src_wav)],
            capture_output=True, text=True).stdout.strip()
        if out != "2":
            return "M"
        p = sp.run(
            ["ffmpeg", "-hide_banner", "-i", str(src_wav),
             "-af", "ebur128=metadata=1", "-f", "null", "-"],
            capture_output=True, text=True).stderr
    except Exception:
        return "M"
    return "M"  # 用简化法：见 _lr_rms


def _lr_ratio(wav: Path) -> float:
    """左右声道 RMS 比（0~1，0.5 居中）。"""
    import wave, struct
    try:
        w = wave.open(str(wav))
        ch = w.getnchannels()
        if ch != 2:
            w.close()
            return 0.5
        n = w.getnframes()
        data = w.readframes(n)
        w.close()
    except Exception:
        return 0.5
    samples = struct.unpack(f"<{len(data)//2}h", data[:len(data) - len(data) % 4])
    l = r = 0
    cnt = len(samples) // 2
    if cnt == 0:
        return 0.5
    for i in range(0, cnt * 2, 2):
        l += samples[i] * samples[i]
        r += samples[i + 1] * samples[i + 1]
    l = (l / cnt) ** 0.5
    r = (r / cnt) ** 0.5
    if l + r == 0:
        return 0.5
    return round(l / (l + r), 3)


def _speaker_pos(ratio: float) -> str:
    """文剑=正对居中(~0.5)，进坤=右前方(右声道强, <0.45)，忠哥=右侧(明天不在)"""
    if ratio < 0.45:
        return "R"       # 右侧 → 进坤
    if ratio > 0.55:
        return "L"       # 左侧 → 异常/未预期
    return "M"           # 居中 → 文剑（正对面）


def ingest(audio: Path, *, date: str | None = None, keep_audio: bool = True) -> dict:
    audio = Path(audio).expanduser().resolve()
    if not audio.exists():
        raise FileNotFoundError(audio)

    now = datetime.now()
    date = date or now.strftime("%Y-%m-%d")
    sid = store.new_session_id(now)
    sdir = store.session_dir(date, sid)
    sdir.mkdir(parents=True, exist_ok=True)

    lr = _lr_ratio(audio)
    spk = _speaker_pos(lr)
    wav = store.session_dir(date, sid) / "audio16k.wav"
    to_wav16k(audio, wav)
    if keep_audio:
        shutil.copy2(audio, sdir / f"source{audio.suffix or '.bin'}")

    segments, engine = _run_asr(wav)
    if not segments:
        meta = {
            "session_id": sid,
            "speaker_pos": spk,
            "lr_ratio": lr,
            "date": date,
            "audio": audio.name,
            "duration_ms": 0,
            "segments": 0,
            "kept": 0,
            "review": 0,
            "status": "no_speech",
            "asr": {"engine": engine},
            "llm": {"model": cfg.model_fast},
            "created_at": now.isoformat(timespec="seconds"),
        }
        _write_transcript(sdir / "transcript.md", [])
        store.write_json(sdir / "meta.json", meta)
        store.write_json(sdir / "summary.json", {"session_id": sid, "kept": [], "review": []})
        if not keep_audio:
            wav.unlink(missing_ok=True)
        return meta

    entities = flt.load_entities(cfg.root)
    rows = flt.classify(segments, entities)

    # 逐 ASR 段识别说话人。不能用整段 WAV 的一个结果覆盖多人会话。
    try:
        from . import voiceprint_np as vp
        lib_path = cfg.root.parent / "voiceprints" / "library.json"
        speakers = vp.identify_segments(wav, rows, lib_path)
        for row, who in zip(rows, speakers, strict=False):
            row["speaker"] = who["speaker"]
            row["speaker_conf"] = who["confidence"]
            row["speaker_source"] = who.get("source", "low")
        recognized = sorted({item["speaker"] for item in speakers if item["speaker"] != "其他人"})
        if recognized:
            print(f"[pipeline] 说话人: {', '.join(recognized)}", flush=True)
    except Exception as exc:  # noqa: BLE001
        print(f"[pipeline] 声纹识别跳过: {exc}", flush=True)

    store.append_jsonl(sdir / "segments.jsonl", rows)
    _write_transcript(sdir / "transcript.md", rows)

    kept = [r for r in rows if r["work"] and r["confidence"] >= cfg.filter_threshold]
    review = [
        r
        for r in rows
        if r["work"] and cfg.review_threshold <= r["confidence"] < cfg.filter_threshold
    ]

    meta = {
        "session_id": sid,
        "speaker_pos": spk,
        "lr_ratio": lr,
        "date": date,
        "audio": audio.name,
        "duration_ms": rows[-1]["end_ms"] if rows else 0,
        "segments": len(rows),
        "kept": len(kept),
        "review": len(review),
        "asr": {"engine": engine},
        "llm": {"model": cfg.model_fast},
        "created_at": now.isoformat(timespec="seconds"),
    }
    store.write_json(sdir / "meta.json", meta)
    store.write_json(
        sdir / "summary.json",
        {
            "session_id": sid,
            "kept": [_brief(r) for r in kept],
            "review": [_brief(r) for r in review],
        },
    )
    if not keep_audio:
        wav.unlink(missing_ok=True)
    return meta


def _run_asr(wav):
    """按配置选 ASR：默认阿里云，失败自动回退本地 whisper。"""
    if _cfg.asr_provider == "aliyun":
        try:
            from .asr_aliyun import transcribe_file
            from .hotwords import ensure_vocabulary

            vid = None
            try:
                vid = ensure_vocabulary(flt.load_entities(cfg.root))
            except Exception:
                pass
            segs = transcribe_file(wav, vocabulary_id=vid)
            if segs:
                return segs, "aliyun-nls"
            print("[asr] 阿里云返回空结果，标记为 no_speech，不回退 whisper", flush=True)
            return [], "aliyun-nls-no-speech"
        except Exception as exc:  # noqa: BLE001
            print(f"[asr] 阿里云失败，回退本地 whisper: {exc}", flush=True)
    return transcribe(wav), "whisper.cpp"


def _brief(r: dict) -> dict:
    return {
        "i": r["i"],
        "start_ms": r["start_ms"],
        "end_ms": r["end_ms"],
        "text": r["text"],
        "type": r["type"],
        "project": r["project"],
        "people": r["people"],
        "points": r["points"],
        "actions": r["actions"],
        "confidence": r["confidence"],
        "sensitive": r["sensitive"],
        "speaker": r.get("speaker", "其他人"),
        "speaker_conf": r.get("speaker_conf", 0.0),
    }


def _write_transcript(path: Path, rows: list[dict]) -> None:
    lines = ["# 转录", ""]
    for r in rows:
        mark = "✅" if r["work"] else "·"
        tag = f"[{r['type']}]" if r["work"] else "[闲聊]"
        speaker = r.get("speaker", "其他人")
        lines.append(f"- `{store.ts(r['start_ms'])}` {mark} {tag} [{speaker}] {r['text']}")
    store.atomic_write(path, "\n".join(lines) + "\n")


def load_day(date: str) -> list[dict]:
    """读取某天所有会话的 summary。"""
    base = store.day_dir(date) / "sessions"
    if not base.exists():
        return []
    out = []
    for d in sorted(base.iterdir()):
        f = d / "summary.json"
        if f.exists():
            out.append(json.loads(f.read_text(encoding="utf-8")))
    return out
