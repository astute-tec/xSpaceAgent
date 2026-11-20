"""阿里云智能语音交互 · 录音文件识别（nls-filetrans）。

流程：SubmitTask(POST, RPC 签名) → 轮询 GetTaskResult → 句子列表

注意：音频必须是**公网可访问的 URL（域名，不能是 IP）**，阿里自己去拉。
"""
from __future__ import annotations

import base64
import hashlib
import hmac
import json
import time
import urllib.parse
import urllib.request
import uuid
from datetime import datetime, timezone

from .config import cfg
import os, subprocess, tempfile
from pathlib import Path

DOMAIN_CN = "filetrans.cn-shanghai.aliyuncs.com"
DOMAIN_SG = "filetrans.ap-southeast-1.aliyuncs.com"
REGION_CN = "cn-shanghai"
REGION_SG = "ap-southeast-1"
API_VERSION = "2019-08-23"


class AliyunError(RuntimeError):
    pass


def _enc(s) -> str:
    return urllib.parse.quote(str(s), safe="~")


def _signed(params: dict, method: str = "POST") -> str:
    q = "&".join(f"{_enc(k)}={_enc(v)}" for k, v in sorted(params.items()))
    sts = f"{method}&%2F&{_enc(q)}"
    sig = base64.b64encode(
        hmac.new((cfg.aliyun_secret + "&").encode(), sts.encode(), hashlib.sha1).digest()
    ).decode()
    return f"{q}&Signature={_enc(sig)}"


def _base_params(action: str, region: str) -> dict:
    return {
        "Action": action,
        "Version": API_VERSION,
        "AccessKeyId": cfg.aliyun_key_id,
        "SignatureMethod": "HMAC-SHA1",
        "SignatureVersion": "1.0",
        "SignatureNonce": str(uuid.uuid4()),
        "Timestamp": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "Format": "JSON",
        "RegionId": region,
    }


def _call(action: str, extra: dict, domain: str, region: str, method: str = "POST") -> dict:
    params = _base_params(action, region)
    params.update(extra)
    body = _signed(params, method)
    url = f"https://{domain}/"
    if method == "GET":
        req = urllib.request.Request(f"{url}?{body}", method="GET")
    else:
        req = urllib.request.Request(
            url,
            data=body.encode(),
            method="POST",
            headers={"Content-Type": "application/x-www-form-urlencoded"},
        )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")
        try:
            j = json.loads(detail)
            raise AliyunError(f"{j.get('Code')}: {j.get('Message')}") from exc
        except json.JSONDecodeError:
            raise AliyunError(f"HTTP {exc.code}: {detail[:300]}") from exc


def submit(file_url: str, *, region: str = "cn", vocabulary_id: str | None = None,
           auto_split: bool = True) -> str:
    task = {
        "appkey": cfg.aliyun_appkey,
        "file_link": file_url,
        "version": "4.0",
        "enable_words": False,
        "enable_sample_rate_adaptive": True,
        "auto_split": auto_split,          # 智能分轨：用 ChannelId 区分说话人
    }
    if vocabulary_id:
        task["vocabulary_id"] = vocabulary_id

    domain, rid = (DOMAIN_CN, REGION_CN) if region == "cn" else (DOMAIN_SG, REGION_SG)
    data = _call("SubmitTask", {"Task": json.dumps(task, ensure_ascii=False)}, domain, rid, "POST")
    if data.get("StatusCode") != 21050000 or not data.get("TaskId"):
        raise AliyunError(f"提交失败: {data.get('StatusText')} ({data.get('StatusCode')})")
    return data["TaskId"]


def poll(task_id: str, *, region: str = "cn", timeout: float = 900, interval: float = 3) -> dict:
    domain, rid = (DOMAIN_CN, REGION_CN) if region == "cn" else (DOMAIN_SG, REGION_SG)
    deadline = time.time() + timeout
    while time.time() < deadline:
        data = _call("GetTaskResult", {"TaskId": task_id}, domain, rid, "GET")
        code = data.get("StatusCode")
        if code == 21050000:
            return data
        if code == 21050003:
            return {"Result": {"Sentences": []}, "StatusText": "NO_VALID_FRAGMENT"}
        if code in (21050001, 21050002):       # RUNNING / QUEUEING
            time.sleep(interval)
            continue
        raise AliyunError(f"{data.get('StatusText')} ({code})")
    raise AliyunError("轮询超时")


def transcribe_url(file_url: str, **kw) -> list[dict]:
    """返回 [{"i","start_ms","end_ms","text","speaker"}]"""
    task_id = submit(file_url, **kw)
    data = poll(task_id, **kw)
    sentences = (data.get("Result") or {}).get("Sentences") or []
    out = []
    for s in sentences:
        text = (s.get("Text") or "").strip()
        if not text:
            continue
        out.append(
            {
                "i": len(out),
                "start_ms": int(s.get("BeginTime", 0)),
                "end_ms": int(s.get("EndTime", 0)),
                "text": text,
                "speaker": f"spk{s.get('ChannelId', 0)}",
            }
        )
    return out


# ---------------- 一句话识别（分片） ----------------
# 「录音文件识别」接口在部分账号/地域不可用，改用 ≤55s 分片调用「一句话识别」。
# 质量与录音文件识别一致，代价是要自己切段。

_TOKEN_CACHE: dict[str, tuple[str, float]] = {}


def token(region: str = "cn-shanghai", ttl: int = 3000) -> str:
    import time as _t
    cached = _TOKEN_CACHE.get(region)
    if cached and cached[1] - _t.time() > 60:
        return cached[0]
    params = {
        "Action": "CreateToken", "Version": "2019-02-28",
        "AccessKeyId": cfg.aliyun_key_id, "SignatureMethod": "HMAC-SHA1",
        "SignatureVersion": "1.0", "SignatureNonce": str(uuid.uuid4()),
        "Timestamp": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "Format": "JSON", "RegionId": region,
    }
    q = "&".join(f"{_enc(k)}={_enc(v)}" for k, v in sorted(params.items()))
    sts = "GET&%2F&" + _enc(q)
    sig = base64.b64encode(
        hmac.new((cfg.aliyun_secret + "&").encode(), sts.encode(), hashlib.sha1).digest()
    ).decode()
    host = "nls-meta.cn-shanghai.aliyuncs.com"
    with urllib.request.urlopen(f"https://{host}/?{q}&Signature={_enc(sig)}", timeout=15) as r:
        data = json.loads(r.read().decode())
    tok = data.get("Token", {}).get("Id")
    if not tok:
        raise AliyunError(f"取 token 失败: {data}")
    _TOKEN_CACHE[region] = (tok, _t.time() + ttl)
    return tok


def _asr_once(wav_bytes: bytes, tok: str, vocabulary_id: str | None = None) -> str:
    url = ("https://nls-gateway-cn-shanghai.aliyuncs.com/stream/v1/asr"
           f"?appkey={cfg.aliyun_appkey}&format=wav&sample_rate=16000"
           "&enable_punctuation_prediction=true&enable_inverse_text_normalization=true")
    if vocabulary_id:
        url += f"&vocabulary_id={vocabulary_id}"
    req = urllib.request.Request(
        url, data=wav_bytes, method="POST",
        headers={"X-NLS-Token": tok, "Content-Type": "application/octet-stream"},
    )
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            data = json.loads(r.read().decode())
    except urllib.error.HTTPError as exc:
        raise AliyunError(f"一句话识别 HTTP {exc.code}: {exc.read().decode()[:200]}") from exc
    if data.get("status") != 20000000:
        raise AliyunError(f"一句话识别失败: {data.get('message')} ({data.get('status')})")
    return data.get("result", "") or ""


def _split_points(wav: Path, target: int = 50) -> list[float]:
    """按静音找切点，尽量不切在句子中间。"""
    try:
        out = subprocess.run(
            ["ffmpeg", "-hide_banner", "-i", str(wav), "-af", "silencedetect=n=-35dB:d=0.5",
             "-f", "null", "-"],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
        ).stderr
    except Exception:
        return []
    sil = []
    cur = None
    for line in out.splitlines():
        if "silence_start:" in line:
            cur = float(line.split("silence_start:")[1].split()[0])
        elif "silence_end:" in line and cur is not None:
            end = float(line.split("silence_end:")[1].split()[0])
            sil.append((cur + end) / 2)
            cur = None
    dur = float(subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", str(wav)],
        capture_output=True, text=True, encoding="utf-8", errors="replace").stdout.strip() or 0)
    points, t = [], target
    while t < dur - 5:
        near = min(sil, key=lambda x: abs(x - t)) if sil else t
        points.append(near if abs(near - t) < 8 else t)
        t = points[-1] + target
    return points


def transcribe_file(wav: Path, language: str = "zh", vocabulary_id: str | None = None) -> list[dict]:
    """分片调用一句话识别，返回带时间偏移的句子。"""
    tok = token()
    wav = Path(wav)
    dur = float(subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", str(wav)],
        capture_output=True, text=True, encoding="utf-8", errors="replace").stdout.strip() or 0)
    cuts = [0.0] + _split_points(wav) + [dur]
    segs: list[dict] = []
    for i in range(len(cuts) - 1):
        start, end = cuts[i], cuts[i + 1]
        if end - start < 0.5:
            continue
        tmp = Path(tempfile.mkdtemp()) / f"p{i}.wav"
        subprocess.run(
            ["ffmpeg", "-y", "-loglevel", "error", "-ss", f"{start:.2f}", "-t", f"{end-start:.2f}",
             "-i", str(wav), "-ar", "16000", "-ac", "1", "-c:a", "pcm_s16le", str(tmp)],
            capture_output=True)
        text = _asr_once(tmp.read_bytes(), tok, vocabulary_id).strip()
        if not text:
            continue
        # 一段可能有多句，按句号拆开，时间按字数比例分摊
        import re as _re
        text = text.replace("?", "？").replace("!", "！")
        raw = [p.strip() for p in _re.split(r"(?<=[。！？])", text) if p.strip()]
        parts: list[str] = []
        for p in raw:                       # 单句过长时再按逗号切，便于后续过滤
            if len(p) > 60:
                parts.extend(x.strip() for x in _re.split(r"(?<=[，,])", p) if x.strip())
            else:
                parts.append(p)
        total = sum(len(p) for p in parts) or 1
        acc = 0
        for p in parts:
            p_start = int((start + (end - start) * acc / total) * 1000)
            acc += len(p)
            p_end = int((start + (end - start) * acc / total) * 1000)
            t = p.strip()
            if t and t[-1] not in "。！？":
                t += "。"
            segs.append({"i": len(segs), "start_ms": p_start, "end_ms": max(p_end, p_start + 300),
                         "text": t})
    return segs
