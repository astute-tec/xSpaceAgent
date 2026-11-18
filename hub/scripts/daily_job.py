#!/usr/bin/env python3
"""每天 18:30 由 systemd timer 调用：聚合当天所有会话 → 日报 → 推送唐僧。"""
from __future__ import annotations

import sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mywork_hub import pipeline, push, report  # noqa: E402
from mywork_hub.config import cfg  # noqa: E402
from mywork_hub import filter as flt  # noqa: E402


def main() -> int:
    date = datetime.now().strftime("%Y-%m-%d")
    sessions = pipeline.load_day(date)
    if not sessions:
        print(f"[daily] {date} 无会话，跳过")
        return 0
    entities = flt.load_entities(cfg.root)
    daily = report.write_daily(date, sessions, entities)
    print(f"[daily] 日报已生成：{daily.get('one_liner')}")

    result = push.push(date, daily)
    push.save_push_result(date, result)
    if result.get("sent"):
        print("[daily] 已推送唐僧")
    else:
        print(f"[daily] 推送失败：{result.get('error') or result.get('stderr')}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
