"""阿里云热词表：从 Mywork 项目目录名自动生成。"""
from __future__ import annotations

import json
import re
import urllib.request

from .config import cfg


DOMAIN = "nls-slp.cn-shanghai.aliyuncs.com"
VERSION = "2018-11-20"


def _pop(action: str, extra: dict, version: str = "2018-11-20") -> dict:
    from .asr_aliyun import _base_params, _signed   # 复用签名

    params = _base_params(action, cfg.aliyun_region or "cn-shanghai")
    params["Version"] = version
    params.update(extra)
    body = _signed(params, "POST")
    req = urllib.request.Request(
        f"https://{DOMAIN}/", data=body.encode(),
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read().decode())


def extract_words(entities: list[str]) -> list[dict]:
    """目录名 → 热词（拆分出项目简称/公司名，权重默认 3）。"""
    words: dict[str, int] = {}
    for name in entities:
        for part in re.split(r"[_\-/、，,]", name):
            part = part.strip()
            # 纯英文缩写（POC/UDP/AV1）或 2-20 字的中文词
            # 阿里限制：纯中文 ≤15 字，中英混合 ≤15 字符；权重 2 为推荐值
            if 2 <= len(part) <= 15 and not re.fullmatch(r"[\d.]+", part) \
                    and not re.search(r"[（）()【】\[\]，,。.；;：:]", part):
                words[part] = 2
    return [{"word": w, "weight": v} for w, v in words.items()][:400]


def ensure_vocabulary(entities: list[str], force: bool = False) -> str | None:
    """创建/复用热词表，返回 vocabulary_id。"""
    if not cfg.aliyun_key_id:
        return None
    words = extract_words(entities)
    if not words:
        return None

    marker = Path(cfg.root) / ".hotword-id"
    if not force and marker.exists():
        return marker.read_text().strip() or None

    try:
        # 业务专属热词：CreateAsrVocab（nls-slp, 2018-11-20），WordWeights 是 {词: 权重} 映射
        clean = {w["word"]: w["weight"] for w in words
                 if w["word"].isascii() or all('\u4e00' <= c <= '\u9fff' for c in w["word"])}
        data = _pop("CreateAsrVocab", {
            "Name": f"mywork-{datetime.now():%Y%m%d%H%M}",
            "Description": "auto from Mywork project dirs",
            "WordWeights": json.dumps(clean, ensure_ascii=False),
        }, version="2018-11-20")
        vid = data.get("VocabId") or (data.get("Data") or {}).get("VocabId")
        if vid:
            marker.parent.mkdir(parents=True, exist_ok=True)
            marker.write_text(vid)
            return vid
    except Exception:
        return None
    return None


from datetime import datetime  # noqa: E402
from pathlib import Path  # noqa: E402
