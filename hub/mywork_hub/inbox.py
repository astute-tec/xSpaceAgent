"""Durable device-audio queue. Audio is fsynced before its receipt is committed.

At-least-once delivery, deterministic session IDs, bounded retry backoff. SQLite
is an index/journal; original audio and session files remain recoverable.
"""
from __future__ import annotations

import hashlib
import json
import os
import sqlite3
import threading
import time
from contextlib import contextmanager
from pathlib import Path


class SegmentConflict(ValueError):
    pass


class DurableInbox:
    def __init__(self, root: Path):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self.database = self.root / ".recording-jobs.sqlite3"
        self.lock = threading.RLock()
        with self.connect() as db:
            db.executescript("""
                CREATE TABLE IF NOT EXISTS jobs (
                    id TEXT PRIMARY KEY, path TEXT NOT NULL, sha256 TEXT NOT NULL,
                    bytes INTEGER NOT NULL, captured_at INTEGER, received_at REAL NOT NULL,
                    state TEXT NOT NULL DEFAULT 'pending', attempts INTEGER NOT NULL DEFAULT 0,
                    next_at REAL NOT NULL DEFAULT 0, error TEXT NOT NULL DEFAULT '',
                    session_id TEXT NOT NULL, date TEXT);
                CREATE TABLE IF NOT EXISTS reports (
                    date TEXT PRIMARY KEY, generation INTEGER NOT NULL DEFAULT 1,
                    completed INTEGER NOT NULL DEFAULT 0, next_at REAL NOT NULL DEFAULT 0,
                    error TEXT NOT NULL DEFAULT '');
            """)

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.database, timeout=15)
        db.row_factory = sqlite3.Row
        db.execute("PRAGMA journal_mode=WAL")
        db.execute("PRAGMA synchronous=FULL")
        try:
            with db:
                yield db
        finally:
            db.close()

    @staticmethod
    def session_id(segment_id: str) -> str:
        return "device_" + hashlib.sha256(segment_id.encode()).hexdigest()[:24]

    def receive(self, segment_id: str, data: bytes, suffix: str,
                captured_at: int | None = None) -> dict:
        digest = hashlib.sha256(data).hexdigest()
        path = self.root / f"dev_{segment_id}{suffix}"
        with self.lock, self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT * FROM jobs WHERE id=?", (segment_id,)).fetchone()
            duplicate = row is not None or path.exists()
            if row and (row["sha256"] != digest or row["bytes"] != len(data)):
                raise SegmentConflict("segment id conflict")
            if path.exists():
                if path.stat().st_size != len(data) or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                    raise SegmentConflict("segment id conflict")
            else:
                part = path.with_name(f".{path.name}.{os.getpid()}.{threading.get_ident()}.part")
                try:
                    with part.open("wb") as stream:
                        stream.write(data)
                        stream.flush()
                        os.fsync(stream.fileno())
                    os.replace(part, path)
                    # Windows 不支持打开目录句柄（PermissionError），仅 POSIX 做目录 fsync。
                    if os.name == "posix":
                        fd = os.open(self.root, os.O_RDONLY)
                        try:
                            os.fsync(fd)
                        finally:
                            os.close(fd)
                finally:
                    part.unlink(missing_ok=True)
            db.execute("""INSERT OR IGNORE INTO jobs
                (id,path,sha256,bytes,captured_at,received_at,session_id)
                VALUES (?,?,?,?,?,?,?)""",
                (segment_id, str(path), digest, len(data), captured_at,
                 time.time(), self.session_id(segment_id)))
        return {"ok": True, "id": segment_id, "bytes": len(data),
                "sha256": digest, "duplicate": duplicate, "durable": True}

    def recover(self, known_processed: set[str] | None = None) -> int:
        """Only called once on startup, before the single worker is started."""
        known_processed = known_processed or set()
        with self.connect() as db:
            db.execute("UPDATE jobs SET state='pending',next_at=0 WHERE state='processing'")
        imported = 0
        for path in sorted(self.root.glob("dev_*")):
            if path.suffix not in (".wav", ".opus") or not path.is_file():
                continue
            ident = path.stem.removeprefix("dev_")
            with self.connect() as db:
                if db.execute("SELECT 1 FROM jobs WHERE id=?", (ident,)).fetchone():
                    continue
            captured = None
            last = ident.rsplit("_", 1)[-1]
            if last.isdecimal() and 1700000000 <= int(last) <= time.time() + 300:
                captured = int(last)
            self.receive(ident, path.read_bytes(), path.suffix, captured)
            if path.name in known_processed:
                with self.connect() as db:
                    db.execute("UPDATE jobs SET state='done' WHERE id=?", (ident,))
            imported += 1
        return imported

    def claim(self) -> dict | None:
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("""SELECT * FROM jobs WHERE state IN ('pending','retry')
                AND next_at<=? ORDER BY received_at,id LIMIT 1""", (time.time(),)).fetchone()
            if not row:
                return None
            db.execute("UPDATE jobs SET state='processing',attempts=attempts+1 WHERE id=?", (row["id"],))
            item = dict(row)
            item["attempts"] += 1
            return item

    def complete(self, item: dict, date: str, report_delay: float = 300) -> None:
        with self.connect() as db:
            db.execute("UPDATE jobs SET state='done',date=?,error='' WHERE id=?", (date, item["id"]))
            db.execute("""INSERT INTO reports(date,next_at) VALUES (?,?)
                ON CONFLICT(date) DO UPDATE SET generation=generation+1,
                next_at=CASE WHEN generation=completed THEN excluded.next_at ELSE next_at END""",
                (date, time.time() + report_delay))

    def fail(self, item: dict, error: str) -> None:
        delay = min(3600, 10 * 2 ** min(item["attempts"] - 1, 8))
        with self.connect() as db:
            db.execute("UPDATE jobs SET state='retry',next_at=?,error=? WHERE id=?",
                       (time.time() + delay, error[:500], item["id"]))

    def due_report(self) -> dict | None:
        with self.connect() as db:
            row = db.execute("""SELECT * FROM reports WHERE generation>completed
                AND next_at<=? ORDER BY next_at LIMIT 1""", (time.time(),)).fetchone()
            return dict(row) if row else None

    def finish_report(self, report: dict, error: str = "") -> None:
        with self.connect() as db:
            if error:
                db.execute("UPDATE reports SET next_at=?,error=? WHERE date=?",
                           (time.time()+60, error[:500], report["date"]))
            else:
                db.execute("UPDATE reports SET completed=?,error='',next_at=? WHERE date=?",
                           (report["generation"], time.time()+300, report["date"]))

    def stats(self) -> dict:
        with self.connect() as db:
            return {row["state"]: row["n"] for row in
                    db.execute("SELECT state,COUNT(*) AS n FROM jobs GROUP BY state")}


def processed_audio_names(work_dir: Path) -> set[str]:
    names = set()
    for path in Path(work_dir).glob("????-??/????-??-??/sessions/*/meta.json"):
        try:
            item = json.loads(path.read_text())
            if item.get("audio") and path.with_name("summary.json").exists():
                names.add(item["audio"])
        except (OSError, ValueError):
            continue
    return names
