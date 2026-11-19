from __future__ import annotations

import json
import os
import sys
import tempfile
import threading
import unittest
import wave
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mywork_hub import voiceprint_np as vp  # noqa: E402


def voice_like(freq: float, seconds: float = 8.0) -> np.ndarray:
    t = np.arange(int(vp.SR * seconds), dtype=np.float32) / vp.SR
    carrier = (
        np.sin(2 * np.pi * freq * t)
        + 0.42 * np.sin(2 * np.pi * freq * 2.03 * t)
        + 0.18 * np.sin(2 * np.pi * freq * 3.01 * t)
    )
    syllables = 0.18 + 0.82 * (np.sin(2 * np.pi * 2.7 * t) > -0.25).astype(np.float32)
    fade = np.minimum(1.0, np.minimum(t * 8.0, (seconds - t) * 8.0))
    return (0.16 * carrier * syllables * np.maximum(fade, 0.0)).astype(np.float32)


def write_wav(path: Path, samples: np.ndarray) -> None:
    pcm = np.clip(samples * 32767.0, -32768, 32767).astype("<i2")
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(vp.SR)
        wav.writeframes(pcm.tobytes())


class VoiceprintTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.lib = self.root / "voiceprints" / "library.json"
        self.a = self.root / "a.wav"
        self.b = self.root / "b.wav"
        write_wav(self.a, voice_like(185.0))
        write_wav(self.b, voice_like(315.0))

    def tearDown(self):
        self.temp.cleanup()

    def test_silence_is_rejected(self):
        silent = self.root / "silent.wav"
        write_wav(silent, np.zeros(vp.SR * 8, dtype=np.float32))
        result = vp.enroll(silent, 0, self.lib)
        self.assertFalse(result["ok"])
        self.assertIn("声音太小", result["msg"])
        self.assertFalse(self.lib.exists())

    def test_enroll_uses_multiple_templates_and_private_permissions(self):
        result = vp.enroll(self.a, 0, self.lib, name="测试甲")
        self.assertTrue(result["ok"])
        self.assertGreaterEqual(result["samples"], 2)
        data = json.loads(self.lib.read_text(encoding="utf-8"))
        self.assertEqual(data["测试甲"]["slot"], 0)
        self.assertEqual(len(data["测试甲"]["embeddings"]), result["samples"])
        self.assertEqual(os.stat(self.lib).st_mode & 0o777, 0o600)
        self.assertEqual(os.stat(self.lib.parent).st_mode & 0o777, 0o700)

    def test_same_slot_replaces_and_unenroll_removes(self):
        self.assertTrue(vp.enroll(self.a, 0, self.lib, name="旧名字")["ok"])
        self.assertTrue(vp.enroll(self.b, 0, self.lib, name="新名字")["ok"])
        data = vp.load_library(self.lib)
        self.assertNotIn("旧名字", data)
        self.assertIn("新名字", data)
        result = vp.unenroll(0, self.lib)
        self.assertTrue(result["ok"])
        self.assertEqual(vp.load_library(self.lib), {})

    def test_concurrent_enroll_keeps_valid_library(self):
        results = []

        def enroll(path, slot, name):
            results.append(vp.enroll(path, slot, self.lib, name=name))

        threads = [
            threading.Thread(target=enroll, args=(self.a, 0, "甲")),
            threading.Thread(target=enroll, args=(self.b, 1, "乙")),
        ]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertTrue(all(result["ok"] for result in results))
        data = json.loads(self.lib.read_text(encoding="utf-8"))
        self.assertEqual({info["slot"] for info in data.values()}, {0, 1})

    def test_rename_preserves_all_voice_templates_and_metadata(self):
        self.assertTrue(vp.enroll(self.a, 0, self.lib)["ok"])
        before = vp.load_library(self.lib)["成员1"]
        result = vp.rename(0, "王阿忠", self.lib)
        self.assertTrue(result["ok"])
        after = vp.load_library(self.lib)
        self.assertNotIn("成员1", after)
        self.assertEqual(after["王阿忠"], {**before, "name": "王阿忠"})
        self.assertFalse(vp.rename(1, "张文剑", self.lib)["ok"])
        self.assertEqual(vp.load_library(self.lib), after)

    def test_segments_are_identified_independently(self):
        self.assertTrue(vp.enroll(self.a, 0, self.lib, name="甲")["ok"])
        self.assertTrue(vp.enroll(self.b, 1, self.lib, name="乙")["ok"])
        combined = self.root / "combined.wav"
        samples = np.concatenate([voice_like(185.0, 4.0), voice_like(315.0, 4.0)])
        write_wav(combined, samples)
        rows = [
            {"start_ms": 0, "end_ms": 3500},
            {"start_ms": 4500, "end_ms": 8000},
        ]
        result = vp.identify_segments(combined, rows, self.lib)
        self.assertEqual([item["speaker"] for item in result], ["甲", "乙"])


if __name__ == "__main__":
    unittest.main()
