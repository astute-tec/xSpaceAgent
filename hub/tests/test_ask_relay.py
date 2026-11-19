from __future__ import annotations

import http.client
import importlib.util
import io
import json
import sys
import threading
import time
import unittest
import wave
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
spec = importlib.util.spec_from_file_location("relay_under_test", Path(__file__).resolve().parents[1] / "scripts/public_relay.py")
relay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(relay)


class AskRelayTests(unittest.TestCase):
    def setUp(self):
        relay.ASK_JOBS.clear()
        relay.RATE.clear()
        self.auth = patch.object(relay, "DEVICE_TOKEN", "unit-test-token")
        self.auth.start()
        self.server = relay.ThreadingHTTPServer(("127.0.0.1", 0), relay.Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.auth.stop()

    def request(self, method, path, body=None, *, authorized=True, status_only=False):
        client = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=3)
        headers = {"X-Async-Response": "1"}
        if status_only: headers["X-Ask-Status"] = "1"
        if authorized: headers["Authorization"] = "Bearer unit-test-token"
        client.request(method, path, body=body, headers=headers)
        response = client.getresponse()
        result = response.status, response.read(), response.getheader("Content-Type")
        client.close()
        return result

    def test_async_returns_before_worker_and_polls_audio(self):
        entered, release = threading.Event(), threading.Event()
        def worker(job_id, data):
            self.assertEqual(data, b"audio")
            with relay.ASK_JOBS_LOCK: relay.ASK_JOBS[job_id]["stage"] = "tangseng"
            entered.set()
            release.wait(2)
            with relay.ASK_JOBS_LOCK:
                relay.ASK_JOBS[job_id].update(state="done", audio=b"\x00" * 640)
        with patch.object(relay, "process_ask", worker):
            try:
                status, body, _ = self.request("POST", "/v1/ask", b"audio")
                self.assertEqual(status, 202)
                job = json.loads(body)["job"]
                self.assertTrue(entered.wait(1))
                status, body, _ = self.request("GET", "/v1/ask/" + job)
                self.assertEqual(status, 202)
                self.assertEqual(json.loads(body)["stage"], "tangseng")
                release.set()
                deadline = time.monotonic() + 1
                while relay.ASK_JOBS[job]["state"] != "done" and time.monotonic() < deadline:
                    time.sleep(0.01)
                status, body, content_type = self.request("GET", "/v1/ask/" + job)
                self.assertEqual((status, len(body)), (200, 640))
                self.assertIn("audio/pcm", content_type)
            finally:
                release.set()

    def test_jobs_and_metadata_require_authentication(self):
        for path in ("/v1/ask/anything", "/v1/voiceprints"):
            self.assertEqual(self.request("GET", path, authorized=False)[0], 401)
        self.assertEqual(self.request("GET", "/v1/ask/unknown")[0], 404)

    def test_text_is_available_before_tts_finishes_and_after_failure(self):
        entered, release = threading.Event(), threading.Event()
        job = "text-first"
        relay.ASK_JOBS[job] = {"state": "working", "stage": "received", "created": time.monotonic()}
        def tts(answer):
            entered.set()
            release.wait(2)
            raise RuntimeError("TTS unavailable")
        with patch("mywork_hub.asr.to_wav16k"), patch.object(relay, "transcribe_file", return_value=[{"text": "工作进展？"}]), patch.object(relay.askmod, "answer_question", return_value="项目已经完成联调，下一步进行验收。"), patch.object(relay.askmod, "tts", side_effect=tts):
            worker = threading.Thread(target=relay.process_ask, args=(job, b"audio"))
            worker.start()
            try:
                self.assertTrue(entered.wait(1))
                status, body, content_type = self.request("GET", "/v1/ask/" + job, status_only=True)
                result = json.loads(body)
                self.assertEqual((status, result["stage"], result["state"]), (202, "tts", "working"))
                self.assertIn("已经完成联调", result["answer"])
                self.assertIn("application/json", content_type)
                self.assertEqual(self.request("GET", "/v1/ask/" + job, status_only=True, authorized=False)[0], 401)
            finally:
                release.set()
                worker.join(3)
        status, body, _ = self.request("GET", "/v1/ask/" + job, status_only=True)
        self.assertEqual(json.loads(body)["state"], "failed")
        self.assertIn("已经完成联调", json.loads(body)["answer"])

    def test_completed_status_is_json_and_old_audio_protocol_is_preserved(self):
        relay.ASK_JOBS["done"] = {"state": "done", "stage": "done", "answer": "已完成。", "audio": b"\x00" * 640}
        status, body, content_type = self.request("GET", "/v1/ask/done", status_only=True)
        self.assertEqual((status, json.loads(body)["state"]), (202, "done"))
        self.assertIn("已完成", json.loads(body)["answer"])
        self.assertIn("application/json", content_type)
        self.assertEqual(self.request("GET", "/v1/ask/done")[:2], (200, b"\x00" * 640))

    def test_voiceprint_metadata_does_not_expose_embeddings(self):
        entry = {"slot": 0, "name": "测试成员", "embedding": [0.3], "embeddings": [[0.3]]}
        with patch("mywork_hub.voiceprint_np.load_library", return_value={"测试成员": entry}):
            status, body, _ = self.request("GET", "/v1/voiceprints")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body), {"ok": True, "slots": [{"slot": 0, "name": "测试成员"}]})

    def test_no_speech_never_calls_tangseng_or_tts(self):
        job = "silent"
        relay.ASK_JOBS[job] = {"state": "working", "stage": "received", "created": time.monotonic()}
        with patch("mywork_hub.asr.to_wav16k"), patch.object(relay, "transcribe_file", return_value=[]), patch.object(relay.askmod, "answer_question") as answer:
            relay.process_ask(job, b"audio")
            answer.assert_not_called()
        status, body, _ = self.request("GET", "/v1/ask/" + job)
        self.assertEqual(status, 422)
        self.assertEqual(json.loads(body)["stage"], "transcribing")

    def test_worker_converts_tts_to_device_pcm(self):
        buf = io.BytesIO()
        with wave.open(buf, "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(16000)
            wav.writeframes(b"\x01\x00" * 1600)
        job = "working"
        relay.ASK_JOBS[job] = {"state": "working", "stage": "received", "created": time.monotonic()}
        def answer(question, progress):
            self.assertEqual(question, "工作情况？")
            progress("tangseng")
            return "已经完成。"
        with patch("mywork_hub.asr.to_wav16k"), patch.object(relay, "transcribe_file", return_value=[{"text": "工作情况？"}]), patch.object(relay.askmod, "answer_question", side_effect=answer), patch.object(relay.askmod, "tts", return_value=buf.getvalue()):
            relay.process_ask(job, b"audio")
        self.assertEqual(relay.ASK_JOBS[job]["state"], "done")
        self.assertEqual(len(relay.ASK_JOBS[job]["audio"]), 6400)

    def test_stream_delivers_first_audio_before_synthesis_finishes(self):
        first, release = threading.Event(), threading.Event()
        job = "stream-test"
        relay.ASK_JOBS[job] = {"state": "working", "stage": "received", "created": time.monotonic(), "streaming": True}
        def synthesize(answer):
            yield b"\x01\x00" * 320
            first.set()
            release.wait(3)
            yield b"\x02\x00" * 320
        with patch("mywork_hub.asr.to_wav16k"), patch.object(relay, "transcribe_file", return_value=[{"text": "进展？"}]), patch.object(relay.askmod, "answer_question", return_value="已完成"), patch.object(relay.askmod, "iter_tts_pcm", side_effect=synthesize):
            worker = threading.Thread(target=relay.process_ask, args=(job, b"audio"))
            worker.start()
            client = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=2)
            try:
                self.assertTrue(first.wait(1))
                client.request("GET", "/v1/ask/"+job+"/audio", headers={"Authorization": "Bearer unit-test-token"})
                response = client.getresponse()
                self.assertEqual(response.status, 200)
                self.assertIn("channels=1", response.getheader("Content-Type"))
                self.assertEqual(response.read(640), b"\x01\x00"*320)
                self.assertEqual(relay.ASK_JOBS[job]["state"], "working")
                release.set()
                self.assertEqual(response.read(), b"\x02\x00"*320)
            finally:
                release.set(); client.close(); worker.join(3)
        self.assertEqual(relay.ASK_JOBS[job]["state"], "done")
        self.assertEqual(len(relay.ASK_JOBS[job]["audio"]), 2560)

    def test_upload_receipt_is_durable_and_hash_checked(self):
        import tempfile
        import hashlib
        buf = io.BytesIO()
        with wave.open(buf, "wb") as wav:
            wav.setnchannels(1); wav.setsampwidth(2); wav.setframerate(16000)
            wav.writeframes(b"\x01\x00"*1600)
        audio=buf.getvalue()
        with tempfile.TemporaryDirectory() as directory, patch.object(relay,"INBOX",Path(directory)):
            client=http.client.HTTPConnection("127.0.0.1",self.server.server_port,timeout=3)
            headers={"Authorization":"Bearer unit-test-token","X-Segment-Id":"q_123",
                     "X-Content-SHA256":hashlib.sha256(audio).hexdigest()}
            for expected_duplicate in (False,True):
                client.request("POST","/v1/upload",body=audio,headers=headers)
                response=client.getresponse(); receipt=json.loads(response.read())
                self.assertEqual(response.status,202)
                self.assertTrue(receipt["durable"])
                self.assertEqual(receipt["duplicate"],expected_duplicate)
                self.assertEqual(receipt["sha256"],hashlib.sha256(audio).hexdigest())
            self.assertEqual(relay.recording_inbox().stats(),{"pending":1})
            headers["X-Content-SHA256"]="0"*64
            client.request("POST","/v1/upload",body=audio,headers=headers)
            response=client.getresponse(); response.read()
            self.assertEqual(response.status,400)
            client.close()

    def test_midstream_failure_is_not_reported_as_complete_audio(self):
        job = "interrupted-stream"
        relay.ASK_JOBS[job] = {"state": "working", "stage": "received", "created": time.monotonic(), "streaming": True}
        def synthesize(answer):
            yield b"\x01\x00" * 320
            raise RuntimeError("upstream interrupted")
        with patch("mywork_hub.asr.to_wav16k"), patch.object(relay, "transcribe_file", return_value=[{"text": "进展？"}]), patch.object(relay.askmod, "answer_question", return_value="文字仍保留"), patch.object(relay.askmod, "iter_tts_pcm", side_effect=synthesize):
            relay.process_ask(job, b"audio")
        client = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=2)
        try:
            client.request("GET", "/v1/ask/" + job + "/audio", headers={"Authorization": "Bearer unit-test-token"})
            response = client.getresponse()
            self.assertEqual(response.status, 200)
            with self.assertRaises(http.client.IncompleteRead) as caught:
                response.read()
            self.assertEqual(caught.exception.partial, b"\x01\x00" * 320)
        finally:
            client.close()
        self.assertEqual(relay.ASK_JOBS[job]["state"], "failed")
        self.assertEqual(relay.ASK_JOBS[job]["answer"], "文字仍保留")


if __name__ == "__main__":
    unittest.main()
