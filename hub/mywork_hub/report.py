"""日报聚合与渲染。"""
from __future__ import annotations

import json
from datetime import datetime
from pathlib import Path

from . import store
from .config import cfg
from .llm import chat_json

SYSTEM = """你是忠哥的工作秘书。下面是某一天从办公室语音里筛出来的工作片段（已按时间排序，带时间戳）。

请把它们整理成一份"今天的工作日报"，要求：
1. one_liner：一句话说清今天主要在推进什么（不要罗列，要有判断）。
2. projects：按项目/客户归并。同一件事说了多遍只留一次；按重要性排序。
   - status：进展（陈述句，不要"讨论了"这种空话，要写结论）
   - decisions：明确拍板的决定；没有就空数组
   - risks：明确的阻塞或风险；没有就空数组
   - todos：明确的待办，必须能落到人或有期限；模糊的不要写
3. todos：把各项目的待办汇总去重。
4. suggestions：明天/接下来的建议，最多 3 条，要具体。
5. 不要编造片段里没有的信息；片段里含糊的地方，宁可略过。
6. [说话人:姓名] 来自逐段声纹结果；不得根据座位、职责或谈话内容猜测身份。
   [说话人:其他人] 必须保留为其他人，不能强行对应已登记成员。
   发言人不一定是任务负责人；没有明确指派，不可把发言人自动填为负责人。

只输出 JSON：
{"one_liner":"","projects":[{"name":"","status":[""],"decisions":[""],"risks":[""],"todos":[{"what":"","owner":"","due":""}]}],"todos":[{"what":"","owner":"","due":"","project":""}],"suggestions":[""]}"""


def _rhythm(date: str, sessions: list[dict]) -> dict | None:
    """语速/发言分布（阿里/whisper 都带时间戳，直接可算）。"""
    speeds, total_chars, total_sec = [], 0, 0.0
    for ses in sessions:
        for seg in ses.get("kept", []):
            sec = (seg.get("end_ms", 0) - seg.get("start_ms", 0)) / 1000
            if sec <= 0.3:
                continue
            n = len(seg["text"])
            speeds.append(n / sec * 60)
            total_chars += n
            total_sec += sec
    if total_sec < 10 or not speeds:
        return None
    speeds.sort()
    return {
        "avg_speed": round(sum(speeds) / len(speeds)),
        "max_speed": round(speeds[-1]),
        "talk_seconds": round(total_sec),
    }


def build_daily(date: str, sessions: list[dict], entities: list[str] | None = None) -> dict:
    lines: list[str] = []
    for s in sessions:
        spk = s.get("speaker_pos") or s.get("meta", {}).get("speaker_pos") or ""
        tag = f"[位置:{spk}] " if spk and spk != "M" else ""
        for seg in s.get("kept", []):
            t = store.ts(seg["start_ms"])
            proj = f"[{seg['project']}]" if seg.get("project") else ""
            speaker = seg.get("speaker", "其他人")
            lines.append(f"{s.get('captured_at', '')} {t} [说话人:{speaker}] {proj} {seg['text']}")
        for seg in s.get("review", []):
            t = store.ts(seg["start_ms"])
            lines.append(f"{s.get('captured_at', '')} {t} [说话人:{seg.get('speaker', '其他人')}] (存疑) {seg['text']}")

    if not lines:
        return {
            "date": date,
            "one_liner": "今天没有记录到工作内容",
            "projects": [],
            "todos": [],
            "suggestions": [],
            "session_count": len(sessions),
            "generated_at": datetime.now().isoformat(timespec="seconds"),
            "empty": True,
        }

    body = "\n".join(lines)
    if len(body) > 120_000:
        body = body[:120_000] + "\n……（后略）"

    data = chat_json(
        [
            {"role": "system", "content": SYSTEM},
            {"role": "user", "content": f"日期：{date}\n\n片段：\n{body}"},
        ],
        model=cfg.model_pro,
        temperature=0.2,
    )
    data["date"] = date
    data["rhythm"] = _rhythm(date, sessions)
    data["session_count"] = len(sessions)
    data["generated_at"] = datetime.now().isoformat(timespec="seconds")
    data["empty"] = False
    return data


def render_markdown(d: dict) -> str:
    out = [
        f"<!-- voice:auto:start {d['date']} -->",
        f"## 语音纪要 · {d['date']}（{d.get('session_count', 0)} 场）",
        "",
        f"**一句话**：{d.get('one_liner', '')}",
        "",
    ]
    for p in d.get("projects", []):
        out.append(f"### {p.get('name') or '未归类'}")
        for s in p.get("status", []):
            out.append(f"- {s}")
        for s in p.get("decisions", []):
            out.append(f"- **决议**：{s}")
        for s in p.get("risks", []):
            out.append(f"- ⚠️ 风险：{s}")
        for t in p.get("todos", []):
            who = t.get("owner") or "待定"
            due = t.get("due") or "无期限"
            out.append(f"- ☐ {t.get('what')}（{who} / {due}）")
        out.append("")

    todos = d.get("todos", [])
    if todos:
        out.append("### 待办汇总")
        out.append("")
        out.append("| 事项 | 责任人 | 期限 | 项目 |")
        out.append("|---|---|---|---|")
        for t in todos:
            out.append(
                f"| {t.get('what','')} | {t.get('owner') or '待定'} | {t.get('due') or '-'} | {t.get('project') or '-'} |"
            )
        out.append("")

    r = d.get("rhythm")
    if r:
        out += [
            "### 沟通节奏",
            "",
            f"- 有效发言 {r['talk_seconds'] // 60} 分 {r['talk_seconds'] % 60} 秒",
            f"- 平均语速 {r['avg_speed']} 字/分钟（峰值 {r['max_speed']}）",
            "",
        ]

    sug = d.get("suggestions", [])
    if sug:
        out.append("### 建议")
        for s in sug:
            out.append(f"- {s}")
        out.append("")

    out.append(f"<!-- voice:auto:end -->")
    return "\n".join(out) + "\n"


def write_daily(date: str, sessions: list[dict], entities: list[str] | None = None) -> dict:
    daily = build_daily(date, sessions, entities)
    ddir = store.day_dir(date)
    ddir.mkdir(parents=True, exist_ok=True)
    store.write_json(ddir / "daily.json", daily)
    store.atomic_write(ddir / "daily.md", render_markdown(daily))

    tasks = {
        "date": date,
        "generated_at": daily["generated_at"],
        "tasks": [
            {
                "id": f"{date.replace('-', '')}-{i + 1:02d}",
                "what": t.get("what", ""),
                "owner": t.get("owner") or None,
                "due": t.get("due") or None,
                "project": t.get("project") or None,
                "status": "new",
            }
            for i, t in enumerate(daily.get("todos", []))
        ],
    }
    store.write_json(ddir / "tasks.json", tasks)
    return daily
