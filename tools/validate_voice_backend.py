#!/usr/bin/env python3
"""Exercise real async ASR -> TangSeng -> streaming TTS, without printing secrets.

Requires DEVICE_TOKEN in the environment and an explicitly provided test audio
file. This creates a real read-only Q&A request; it does not upload work records.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import time
import urllib.error
import urllib.request


def validate(base_url: str, audio: Path, *, timeout: float = 180) -> dict:
    token = os.environ.get("DEVICE_TOKEN", "")
    if not token:
        raise ValueError("DEVICE_TOKEN must be provided through the environment")
    # Match the ESP-IDF client: the public edge rejects Python's default UA.
    headers = {"Authorization": "Bearer " + token, "User-Agent": "ESP32 HTTP Client/1.0"}
    started = time.monotonic()
    metrics = {"audio_bytes": audio.stat().st_size, "stages": []}

    def elapsed():
        return round(time.monotonic() - started, 3)

    def request(path, *, body=None, extra_headers=None):
        return urllib.request.urlopen(urllib.request.Request(
            base_url.rstrip("/") + path, data=body,
            headers={**headers, **(extra_headers or {})}), timeout=95)

    with request("/v1/ask", body=audio.read_bytes(), extra_headers={
        "X-Async-Response": "1", "X-Stream-Audio": "1", "Content-Type": "audio/ogg"
    }) as response:
        if response.status != 202:
            raise RuntimeError("async question was not accepted")
        endpoint = "/v1/ask/" + json.load(response)["job"]
    last_stage = None
    while time.monotonic() - started < timeout:
        with request(endpoint, extra_headers={"X-Ask-Status": "1"}) as response:
            status = json.load(response)
        if status["stage"] != last_stage:
            metrics["stages"].append({"stage": status["stage"], "seconds": elapsed()})
            last_stage = status["stage"]
        if status.get("answer") and "text_seconds" not in metrics:
            metrics.update(text_seconds=elapsed(), answer_characters=len(status["answer"]))
        if status.get("stream_ready"):
            metrics["state_when_stream_requested"] = status["state"]
            with request(endpoint + "/audio") as response:
                if response.status != 200 or "channels=1" not in response.headers.get("Content-Type", ""):
                    raise RuntimeError("streaming mono PCM response unavailable")
                total = 0
                while chunk := response.read1(2048):
                    if not total:
                        metrics["first_audio_seconds"] = elapsed()
                    total += len(chunk)
            if not total or total % 2:
                raise RuntimeError("empty or incomplete PCM sample")
            with request(endpoint, extra_headers={"X-Ask-Status": "1"}) as response:
                final = json.load(response)
            if final["state"] != "done":
                raise RuntimeError("audio response did not complete successfully")
            metrics.update(audio_pcm_bytes=total, completed_seconds=elapsed(), state="done")
            return metrics
        if status["state"] == "failed":
            raise RuntimeError("question failed at stage " + status["stage"])
        time.sleep(0.5)
    raise TimeoutError("question pipeline exceeded deadline")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True, help="Hub base URL, no query or credentials")
    parser.add_argument("--audio", required=True, type=Path, help="Explicit test audio file")
    parser.add_argument("--output", type=Path, help="Optional metrics JSON path")
    args = parser.parse_args()
    try:
        result = validate(args.url, args.audio)
    except urllib.error.HTTPError as exc:
        raise SystemExit(f"Backend returned HTTP {exc.code}") from None
    text = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text)
    print(text, end="")


if __name__ == "__main__":
    main()
