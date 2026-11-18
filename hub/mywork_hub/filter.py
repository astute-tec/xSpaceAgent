"""工作相关性过滤：规则粗筛 + LLM 精判。"""
from __future__ import annotations

import json
from pathlib import Path

from .config import cfg
from .lexicon import fix as lex_fix
from .llm import chat_json

SYSTEM = """你是办公室语音记录的整理助手。给你若干段来自会议室/工位的聊天转写，逐段判断它是否属于"工作内容"，并抽取信息。

判断标准：
- 工作内容：项目进展、客户沟通、技术方案、排期、报价/合同、运维、人事安排、财务
- 非工作内容：吃饭、天气、家常、八卦、纯语气词、与公司无关的闲聊

规则：
1. 宁可多留，不可错杀——拿不准就判 work=true，但 confidence 降低。
2. 只依据文本判断，不要编造。
3. 若一段里既有闲聊又有工作，按工作算，points 只保留工作部分。
4. 敏感内容（薪酬、人事处分、合同金额、账号密码）标 sensitive 为对应类别，否则 "none"。
5. project 只填已知项目名；不确定就填 null。
6. actions 只填明确的承诺或指派，必须带 owner 或 due 至少一个，模糊表述不要填。

只输出 JSON，不要任何解释：
{"segments":[{"i":0,"work":true,"confidence":0.0-1.0,"type":"项目进展|客户沟通|技术决策|商务报价|财务|人事|运维|其他|闲聊","project":"项目名或null","people":["人名"],"points":["要点"],"actions":[{"what":"","owner":"","due":""}],"sensitive":"none|hr|salary|contract|credential"}]}"""


def load_entities(root: Path) -> list[str]:
    """从 Mywork 目录名抽取项目/客户实体，用于给 LLM 提供线索和纠错。"""
    names: list[str] = []
    for sub in ("01_项目", "00_客户信息管理", "04_竞品分析", "02_客户汇报"):
        base = root / sub
        if not base.exists():
            continue
        for p in base.iterdir():
            if p.is_dir() and not p.name.startswith("."):
                names.append(p.name)
    # 去掉明显噪声
    return [n for n in names if len(n) >= 2][:120]


def _batch(seq: list, size: int):
    for i in range(0, len(seq), size):
        yield seq[i : i + size]


def classify(segments: list[dict], entities: list[str] | None = None) -> list[dict]:
    """逐段判定，返回 work 结果列表（与输入等长）。"""
    if not segments:
        return []

    entities = entities or []
    hint = ""
    if entities:
        hint = "已知的项目/客户名（可用作 project 参考，也可能不出现）：\n" + "、".join(entities)

    results: dict[int, dict] = {}
    for chunk in _batch(segments, 8):
        payload = [
            {"i": s["i"], "t": lex_fix(s["text"]), "sec": round((s["end_ms"] - s["start_ms"]) / 1000, 1)}
            for s in chunk
        ]
        user = f"{hint}\n\n待判定片段：\n{json.dumps(payload, ensure_ascii=False)}"
        data = chat_json(
            [
                {"role": "system", "content": SYSTEM},
                {"role": "user", "content": user},
            ],
            model=cfg.model_fast,
            temperature=0.0,
        )
        for item in data.get("segments", []):
            results[int(item.get("i", -1))] = item

    merged: list[dict] = []
    for s in segments:
        r = results.get(s["i"], {})
        merged.append(
            {
                **s,
                "work": bool(r.get("work", False)),
                "confidence": float(r.get("confidence", 0) or 0),
                "type": r.get("type") or "其他",
                "project": r.get("project"),
                "people": r.get("people") or [],
                "points": r.get("points") or [],
                "actions": r.get("actions") or [],
                "sensitive": r.get("sensitive") or "none",
                "text": lex_fix(r.get("text_fixed") or s["text"]),
            }
        )
    return merged
