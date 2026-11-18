"""落盘：会话目录、原子写、日报路径。"""
from __future__ import annotations

import json
import os
import tempfile
from datetime import datetime
from pathlib import Path

from .config import cfg


def atomic_write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=str(path.parent), prefix=".tmp-")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(text)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


def day_dir(date: str) -> Path:
    """date: YYYY-MM-DD"""
    y, m, _ = date.split("-")
    return cfg.work_dir / f"{y}-{m}" / date


def session_dir(date: str, session_id: str) -> Path:
    return day_dir(date) / "sessions" / session_id


def new_session_id(when: datetime | None = None) -> str:
    when = when or datetime.now()
    return when.strftime("%Y-%m-%d_%H%M") + f"_{os.urandom(2).hex()}"


def write_json(path: Path, obj) -> None:
    atomic_write(path, json.dumps(obj, ensure_ascii=False, indent=2) + "\n")


def append_jsonl(path: Path, rows: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as fh:
        for r in rows:
            fh.write(json.dumps(r, ensure_ascii=False) + "\n")


def ts(ms: int) -> str:
    s = ms // 1000
    return f"{s // 3600:02d}:{(s % 3600) // 60:02d}:{s % 60:02d}"
