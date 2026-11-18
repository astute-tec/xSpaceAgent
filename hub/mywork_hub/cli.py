"""mywork-hub 命令行。"""
from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime
from pathlib import Path

from . import pipeline, push, report
from . import filter as flt
from .config import cfg


def _set_root(value: str | None) -> None:
    if value:
        cfg.root = Path(value).expanduser().resolve()


def cmd_ingest(args) -> int:
    _set_root(args.root)
    meta = pipeline.ingest(Path(args.audio), date=args.date, keep_audio=not args.no_audio)
    print(json.dumps(meta, ensure_ascii=False, indent=2))
    return 0


def cmd_daily(args) -> int:
    _set_root(args.root)
    date = args.date or datetime.now().strftime("%Y-%m-%d")
    sessions = pipeline.load_day(date)
    if not sessions:
        print(f"{date} 没有会话记录", file=sys.stderr)
        return 1
    entities = flt.load_entities(cfg.root)
    daily = report.write_daily(date, sessions, entities)
    frag = push.write_memory_fragment(date, daily)
    print(f"记忆片段: {frag}")
    out = cfg.work_dir / date[:7] / date / "daily.md"
    print(f"已生成: {out}")
    print()
    print(out.read_text(encoding="utf-8"))
    return 0


def cmd_push(args) -> int:
    _set_root(args.root)
    date = args.date or datetime.now().strftime("%Y-%m-%d")
    ddir = cfg.work_dir / date[:7] / date / "daily.json"
    if not ddir.exists():
        print(f"没有 {date} 的日报，先跑 daily", file=sys.stderr)
        return 1
    daily = json.loads(ddir.read_text(encoding="utf-8"))
    result = push.push(date, daily, dry_run=args.dry_run)
    push.save_push_result(date, result)
    if result.get("dry_run"):
        print(result["message"])
    elif result.get("sent"):
        print("已发送给唐僧")
    else:
        print(f"发送失败: {result.get('error') or result.get('stderr')}", file=sys.stderr)
        return 1
    return 0


def cmd_asr_test(args) -> int:
    from .asr_aliyun import transcribe_url

    segs = transcribe_url(args.url, region=args.region)
    print(f"识别到 {len(segs)} 句：")
    for s in segs:
        print(f"  [{s['start_ms']:>7}ms] {s['speaker']}: {s['text']}")
    return 0


def cmd_entities(args) -> int:
    _set_root(args.root)
    names = flt.load_entities(cfg.root)
    print(json.dumps(names, ensure_ascii=False, indent=2))
    return 0


def main(argv=None) -> int:
    p = argparse.ArgumentParser(prog="mywork-hub", description="语音工作助手 · 电脑端流水线")
    sub = p.add_subparsers(dest="cmd", required=True)

    a = sub.add_parser("ingest", help="处理一个音频文件")
    a.add_argument("audio")
    a.add_argument("--date", help="归档日期 YYYY-MM-DD，默认今天")
    a.add_argument("--root", help="Mywork 根目录（默认 ~/Mywork）")
    a.add_argument("--no-audio", action="store_true", help="不保留原始音频")
    a.set_defaults(func=cmd_ingest)

    b = sub.add_parser("daily", help="生成某天日报")
    b.add_argument("--date", help="YYYY-MM-DD，默认今天")
    b.add_argument("--root")
    b.set_defaults(func=cmd_daily)

    d = sub.add_parser("push", help="把日报推送给唐僧")
    d.add_argument("--date")
    d.add_argument("--root")
    d.add_argument("--dry-run", action="store_true", help="只打印消息，不发送")
    d.set_defaults(func=cmd_push)

    e = sub.add_parser("asr-test", help="测试阿里云录音文件识别（传公网音频 URL）")
    e.add_argument("url")
    e.add_argument("--region", default="cn", choices=["cn", "sg"])
    e.set_defaults(func=cmd_asr_test)

    c = sub.add_parser("entities", help="列出抽取到的项目/客户实体")
    c.add_argument("--root")
    c.set_defaults(func=cmd_entities)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
