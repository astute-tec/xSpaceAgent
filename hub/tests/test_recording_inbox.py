from __future__ import annotations

import concurrent.futures
import hashlib
import importlib.util
import io
import json
import sys
import tempfile
import time
import unittest
import wave
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mywork_hub.inbox import DurableInbox, SegmentConflict
from mywork_hub import ask, pipeline, push


class InboxTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.inbox = DurableInbox(self.root / "inbox")

    def tearDown(self):
        self.tmp.cleanup()

    def test_receipt_and_duplicate_survive_restart_and_conflict_is_rejected(self):
        audio = b"OggS" + b"a" * 1000
        receipt = self.inbox.receive("q_1", audio, ".opus", 1795017600)
        self.assertTrue(receipt["durable"])
        self.assertFalse(receipt["duplicate"])
        self.assertEqual(receipt["sha256"], hashlib.sha256(audio).hexdigest())
        restarted = DurableInbox(self.inbox.root)
        self.assertTrue(restarted.receive("q_1", audio, ".opus")["duplicate"])
        self.assertEqual(restarted.stats(), {"pending": 1})
        with self.assertRaises(SegmentConflict):
            restarted.receive("q_1", b"changed", ".opus")
        self.assertEqual(next(restarted.root.glob("dev_*" )).read_bytes(), audio)

    def test_restart_recovers_claimed_and_orphaned_files(self):
        self.inbox.receive("one", b"first", ".wav")
        original = self.inbox.claim()
        (self.inbox.root / "dev_orphan.wav").write_bytes(b"saved before database commit")
        restarted = DurableInbox(self.inbox.root)
        self.assertEqual(restarted.recover(), 1)
        recovered = restarted.claim()
        self.assertEqual(recovered["id"], original["id"])
        self.assertEqual(recovered["session_id"], original["session_id"])

    def test_failure_is_not_lost_and_retries_with_same_identity(self):
        self.inbox.receive("one", b"first", ".wav")
        first = self.inbox.claim()
        self.inbox.fail(first, "ASR timeout")
        self.assertIsNone(self.inbox.claim())
        with patch("mywork_hub.inbox.time.time", return_value=time.time()+20):
            retried = DurableInbox(self.inbox.root).claim()
        self.assertEqual(retried["session_id"], first["session_id"])
        self.assertEqual(retried["attempts"], 2)
        self.assertTrue(Path(retried["path"]).exists())

    def test_report_batches_multiple_segments_and_retries_after_restart(self):
        for ident in ("a", "b"):
            self.inbox.receive(ident, ident.encode(), ".wav")
            item = self.inbox.claim()
            self.inbox.complete(item, "2026-11-19", report_delay=300)
        self.assertIsNone(self.inbox.due_report())
        with patch("mywork_hub.inbox.time.time", return_value=time.time()+301):
            due = DurableInbox(self.inbox.root).due_report()
        self.assertEqual(due["generation"], 2)
        self.inbox.finish_report(due, "唐僧暂不可用")
        self.assertIsNone(self.inbox.due_report())
        self.inbox.finish_report(due)
        with patch("mywork_hub.inbox.time.time", return_value=time.time()+1000):
            self.assertIsNone(self.inbox.due_report())

    def test_concurrent_duplicate_receives_create_one_job(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
            receipts = list(pool.map(lambda _: self.inbox.receive("same", b"x"*100, ".opus"), range(12)))
        self.assertEqual(sum(not x["duplicate"] for x in receipts), 1)
        self.assertEqual(self.inbox.stats(), {"pending": 1})

    def test_pipeline_retries_are_idempotent_and_use_capture_date(self):
        path = self.root / "audio.wav"
        path.write_bytes(b"dummy")
        with patch.object(pipeline.cfg, "root", self.root), patch.object(pipeline, "to_wav16k"), patch.object(pipeline, "_run_asr", return_value=([], "test")) as asr:
            first = pipeline.ingest(path, session_id="device_test", captured_at=1795017600)
            second = pipeline.ingest(path, session_id="device_test", captured_at=1795017600)
        self.assertEqual(first, second)
        self.assertEqual(asr.call_count, 1)
        summaries = list(self.root.rglob("summary.json"))
        self.assertEqual(len(summaries), 1)
        self.assertIn("captured_at", json.loads(summaries[0].read_text()))

    def test_question_goes_to_tangseng_without_normalization_call(self):
        with patch.object(ask, "_daily_context", return_value="facts"), patch.object(ask, "_normalize_question") as normalize, patch.object(ask, "ask_tangseng", return_value="answer") as tangseng:
            self.assertEqual(ask.answer_question("  项目进展？  "), "answer")
        normalize.assert_not_called()
        tangseng.assert_called_once_with("项目进展？", "facts")

    def test_daily_report_ignores_uncommitted_and_corrupt_sessions(self):
        with patch.object(pipeline.cfg, "root", self.root):
            base = pipeline.store.day_dir("2026-11-19") / "sessions"
            for sid in ("complete", "uncommitted", "broken", "wrong_type"):
                directory = base / sid
                directory.mkdir(parents=True)
                (directory / "summary.json").write_text(json.dumps({"session_id": sid}))
                if sid != "uncommitted":
                    (directory / "meta.json").write_text("{}")
            (base / "broken" / "summary.json").write_text("{")
            (base / "wrong_type" / "summary.json").write_text("[]")
            self.assertEqual(pipeline.load_day("2026-11-19"), [{"session_id": "complete"}])

    def test_tangseng_local_delivery_does_not_require_an_ssh_host(self):
        from subprocess import CompletedProcess
        with patch.object(push, "HOST", ""), patch.object(push, "_openclaw_bin", return_value="/usr/bin/openclaw"), patch.object(push, "write_workspace_memory", return_value={"ok": True}), patch.object(push.subprocess, "run", return_value=CompletedProcess([], 0, "{}", "")) as run:
            result = push.push("2026-11-19", {"date": "2026-11-19"})
        self.assertTrue(result["sent"])
        self.assertEqual(run.call_args.args[0][:2], ["/usr/bin/openclaw", "agent"])

    def test_default_workspace_is_agent_memory_not_current_directory(self):
        with patch.dict(push.os.environ, {"OPENCLAW_STATE_DIR": str(self.root)}, clear=True), patch.object(push, "WORKSPACE", None):
            self.assertEqual(push.agent_workspace("tangseng"), self.root / "workspace")

    def test_workspace_uses_installed_agent_config(self):
        config = self.root / "openclaw.json"
        config.write_text(json.dumps({"agents": {"defaults": {"workspace": str(self.root / "shared")}, "list": [{"id": "tangseng", "workspace": str(self.root / "agent")}]}}))
        with patch.dict(push.os.environ, {"OPENCLAW_STATE_DIR": str(self.root)}, clear=True), patch.object(push, "WORKSPACE", None):
            self.assertEqual(push.agent_workspace("tangseng"), self.root / "agent")
            self.assertEqual(push.agent_workspace("other"), self.root / "shared")


if __name__ == "__main__":
    unittest.main()
