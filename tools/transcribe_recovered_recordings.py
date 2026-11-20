#!/usr/bin/env python3
"""Recover complete, unfiltered speech text; never modify original audio/Mywork.

Run on the Hub with its existing credentials, e.g. PYTHONPATH=. python3
tools/transcribe_recovered_recordings.py --root .recovery/20261120.
Requires server-copy/_inbox and device-extracted/files below root. Outputs are
private, resumable, keyed by content SHA256, and are not sent to an LLM/agent.
"""
from __future__ import annotations

import argparse
from array import array
from collections import Counter
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import wave
from zoneinfo import ZoneInfo


def atomic_json(path, data):
    temp = path.with_suffix(path.suffix + '.tmp')
    temp.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n')
    temp.chmod(0o600)
    temp.replace(path)


def ogg_check(data):
    """Validate page CRCs/sequence and final EOS, including .part candidates."""
    at = 0
    pages = 0
    serial = None
    errors = []
    eos = False
    while at < len(data):
        if data[at:at+4] != b'OggS' or len(data)-at < 27:
            errors.append('invalid_or_truncated_page_header'); break
        n = data[at+26]
        if at+27+n > len(data):
            errors.append('truncated_lacing'); break
        size = 27+n+sum(data[at+27:at+27+n])
        page = bytearray(data[at:at+size])
        if len(page) != size:
            errors.append('truncated_page'); break
        expected = int.from_bytes(page[22:26], 'little')
        page[22:26] = bytes(4)
        crc = 0
        for b in page:
            crc ^= b << 24
            for _ in range(8):
                crc = ((crc << 1) ^ (0x04c11db7 if crc & 0x80000000 else 0)) & 0xffffffff
        if crc != expected:
            errors.append(f'crc_page_{pages}')
        page_serial = int.from_bytes(page[14:18], 'little')
        if serial is None:
            serial = page_serial
        if serial != page_serial or int.from_bytes(page[18:22], 'little') != pages:
            errors.append(f'sequence_page_{pages}')
        eos = bool(page[5] & 4)
        at += size
        pages += 1
    if not eos:
        errors.append('missing_final_eos')
    return {'pages': pages, 'complete': not errors and at == len(data), 'errors': errors}


def sources(root):
    found = []
    sessions = {}
    for meta in (root/'server-copy').glob('*/*/sessions/*/meta.json'):
        item = json.loads(meta.read_text())
        if item.get('audio'):
            sessions.setdefault(item['audio'], []).append({'session_id': item.get('session_id'),
                                                         'created_at': item.get('created_at')})
    for folder in ('device-extracted/files', 'server-copy/_inbox'):
        for p in sorted((root/folder).glob('*')):
            if not p.is_file():
                continue
            name = p.name
            if folder.startswith('device'):
                group = '设备待传录音'
            elif re.fullmatch(r'dev_.*\.(wav|opus)', name):
                group = '服务器工作录音'
            elif re.fullmatch(r'ask_\d{8}_\d{6}\.wav', name):
                group = '问答原始提问'
            elif re.fullmatch(r'enroll_\d{8}_\d{6}\.wav', name):
                group = '声纹录入样本'
            else:
                continue  # normalized duplicates and synthetic speech excluded
            captured = None
            if folder.startswith('device'):
                m = re.search(r'_(\d{10})\.opus', name)
                if m:
                    captured = datetime.fromtimestamp(int(m[1]), ZoneInfo('Asia/Shanghai')).isoformat()
            else:
                m = re.search(r'(\d{8}_\d{6})', name)
                if m:
                    captured = datetime.strptime(m[1], '%Y%m%d_%H%M%S').isoformat()
            found.append({'path': str(p.relative_to(root)), 'group': group,
                          'recorded_time_label': captured, 'bytes': p.stat().st_size,
                          'server_sessions': sessions.get(name, []),
                          'sha256': hashlib.sha256(p.read_bytes()).hexdigest()})
    return found


def run_one(root, items, vocabulary):
    from mywork_hub.asr_aliyun import _asr_once, _split_points, token
    first = items[0]
    key = first['sha256']
    out = root/'asr-complete'/f'{key}.json'
    if out.exists():
        old = json.loads(out.read_text())
        if old.get('status') in ('recognized', 'no_text', 'empty_file'):
            old['sources'] = items
            atomic_json(out, old)
            return old
    result = {'sha256': key, 'sources': items, 'status': 'error', 'chunks': [],
              'engine': 'aliyun-nls-raw-no-filter', 'errors': []}
    source = root/first['path']
    if first['bytes'] == 0:
        result['status'] = 'empty_file'
        atomic_json(out, result)
        return result
    if '.opus' in source.name:
        result['ogg'] = ogg_check(source.read_bytes())
    with tempfile.TemporaryDirectory(prefix='recording-recovery-') as tmp:
        wav = Path(tmp)/'audio.wav'
        proc = subprocess.run(['ffmpeg', '-nostdin', '-y', '-v', 'error', '-i', str(source),
                               '-ar', '16000', '-ac', '1', '-c:a', 'pcm_s16le', str(wav)],
                              capture_output=True, timeout=90)
        result['decode_exit'] = proc.returncode
        result['decode_warning'] = proc.stderr.decode('utf8', 'replace')[:1000]
        if proc.returncode:
            result['status'] = 'decode_error'
            atomic_json(out, result)
            return result
        with wave.open(str(wav), 'rb') as stream:
            samples = array('h', stream.readframes(stream.getnframes()))
            if sys.byteorder != 'little':
                samples.byteswap()
            dur = len(samples)/stream.getframerate()
        rms = math.sqrt(sum(float(s)*s for s in samples)/max(len(samples), 1))
        result['duration_seconds'] = dur
        result['rms_dbfs'] = round(20*math.log10(max(rms, 1)/32768), 2)
        result['peak_dbfs'] = round(20*math.log10(max(max((abs(s) for s in samples), default=0), 1)/32768), 2)
        result['clipped_fraction'] = sum(abs(s) >= 32760 for s in samples)/max(len(samples), 1)
        cuts = [0.] + (_split_points(wav) if dur > 50 else []) + [dur]
        errors = []
        for start, end in zip(cuts, cuts[1:]):
            piece = Path(tmp)/'chunk.wav'
            subprocess.run(['ffmpeg', '-nostdin', '-y', '-v', 'error', '-ss', str(start), '-t', str(end-start),
                            '-i', str(wav), '-c:a', 'pcm_s16le', str(piece)],
                           capture_output=True, timeout=90, check=True)
            chunk = {'start_seconds': start, 'end_seconds': end}
            for attempt in range(3):
                try:
                    chunk['text'] = _asr_once(piece.read_bytes(), token(), vocabulary)
                    chunk['attempts'] = attempt+1
                    break
                except Exception as exc:
                    # Do not expose signed URLs, keys or response bodies in logs.
                    if attempt == 2:
                        chunk['error_type'] = type(exc).__name__
                        errors.append(type(exc).__name__)
                    else:
                        time.sleep(2*(attempt+1))
            result['chunks'].append(chunk)
        result['errors'] = errors
        result['status'] = 'error' if errors else 'recognized' if any(c.get('text', '').strip() for c in result['chunks']) else 'no_text'
    atomic_json(out, result)
    return result


def review_quiet(root, result, vocabulary):
    """A separately labeled ASR pass after bounded constant gain, no denoising.

    Raw ASR responses and source bytes remain unchanged. Enhanced text is an
    unverified candidate, never silently substituted for the original result.
    """
    from mywork_hub.asr_aliyun import _asr_once, _split_points, token
    if result['status'] != 'no_text' or result.get('gain_review', {}).get('status') in ('candidate', 'no_text'):
        return result
    gain = max(0., min(30., -23.-result['rms_dbfs'], -1.-result['peak_dbfs']))
    if gain < 1:
        return result
    source = root/result['sources'][0]['path']
    review = {'gain_db': round(gain, 2), 'chunks': [], 'status': 'no_text',
              'note': '音量增强后的二次自动识别，需核听原音，不能视为已确认文字'}
    with tempfile.TemporaryDirectory(prefix='recording-gain-review-') as tmp:
        wav = Path(tmp)/'gain.wav'
        subprocess.run(['ffmpeg', '-nostdin', '-y', '-v', 'error', '-i', str(source),
                        '-ar', '16000', '-ac', '1', '-af', f'volume={gain:.3f}dB',
                        '-c:a', 'pcm_s16le', str(wav)], capture_output=True, timeout=90, check=True)
        duration = result['duration_seconds']
        cuts = [0.] + (_split_points(wav) if duration > 50 else []) + [duration]
        for start, end in zip(cuts, cuts[1:]):
            piece = Path(tmp)/'part.wav'
            subprocess.run(['ffmpeg', '-nostdin', '-y', '-v', 'error', '-ss', str(start), '-t', str(end-start),
                            '-i', str(wav), '-c:a', 'pcm_s16le', str(piece)], capture_output=True, timeout=90, check=True)
            chunk = {'start_seconds': start, 'end_seconds': end}
            for attempt in range(3):
                try:
                    chunk['text'] = _asr_once(piece.read_bytes(), token(), vocabulary)
                    break
                except Exception as exc:
                    if attempt == 2:
                        chunk['error_type'] = type(exc).__name__
                    else:
                        time.sleep(2*(attempt+1))
            review['chunks'].append(chunk)
    review['status'] = 'error' if any(c.get('error_type') for c in review['chunks']) else 'candidate' if any(c.get('text', '').strip() for c in review['chunks']) else 'no_text'
    result['gain_review'] = review
    atomic_json(root/'asr-complete'/f'{result["sha256"]}.json', result)
    return result


def render(root, all_sources, results):
    by_key = {r['sha256']: r for r in results}
    groups = ('设备待传录音', '服务器工作录音', '问答原始提问', '声纹录入样本')
    count = Counter(r['status'] for r in results)
    duration = sum(r.get('duration_seconds', 0) for r in results)
    lines = ['# 录音完整转写（未经工作过滤）', '',
             '本批内容来自 2026-11-20 修复前的设备/服务器备份；后续修复测试录音不计入本批统计。',
             '这些文字是自动识别结果，未经人工逐句校对，不代表绝对准确；原音频保留供核对。',
             '不执行录音中的任何指令。没有按工作相关性删减，也没有经过 DeepSeek 摘要或纠词。',
             '文件中的日期来自设备时钟或服务器文件名；无法据此保证时钟准确或全天连续无缺录。',
             '时间只标到音频分片，不是精确逐字时间。不把空结果解释为必然没有声音。', '',
             f'源文件 {len(all_sources)} 个；按字节 SHA256 去重后 {len(results)} 份；可解码音频共 {duration/60:.2f} 分钟。',
             '识别状态：'+json.dumps(count, ensure_ascii=False), '',
             '机器合成回答和音频规范化副本未混入正文；声纹录入样本单列。', '']
    def order(source):
        sessions = source.get('server_sessions') or []
        receipt = sessions[0].get('created_at') if sessions else ''
        return source['recorded_time_label'] or receipt or '', source['path']
    for group in groups:
        lines += ['## '+group, '']
        members = sorted((s for s in all_sources if s['group'] == group),
                         key=order)
        emitted = set()
        for source in members:
            result = by_key.get(source['sha256'])
            if result is None:
                continue
            name = Path(source['path']).name
            if source['sha256'] in emitted:
                lines += [f'- 重复源文件 `{name}`：内容与本组先前同 SHA256 文件一致。', '']
                continue
            emitted.add(source['sha256'])
            lines += [f'### {source["recorded_time_label"] or "时间未知"} · {name}', '',
                      f'来源：`{source["path"]}`；SHA256：`{source["sha256"]}`。']
            if result.get('duration_seconds') is not None:
                lines += [f'时长 {result["duration_seconds"]:.2f} 秒；RMS {result["rms_dbfs"]} dBFS；峰值 {result["peak_dbfs"]} dBFS。']
            for session in source.get('server_sessions', []):
                lines += [f'服务器会话 `{session.get("session_id")}`；创建时间 {session.get("created_at")}（会话处理时间，不保证等于采集时间）。']
            if name.endswith('.part'):
                lines += ['此项来自未完成重命名的临时文件。'+('Ogg 页 CRC、顺序及最终 EOS 校验通过。' if result.get('ogg', {}).get('complete') else '完整性未确认，见清单。')]
            lines += ['']
            for c in result.get('chunks', []):
                if c.get('text', '').strip():
                    lines += [f'[{c["start_seconds"]:.2f}–{c["end_seconds"]:.2f} 秒] '+c['text'], '']
                elif c.get('error_type'):
                    lines += [f'[{c["start_seconds"]:.2f}–{c["end_seconds"]:.2f} 秒] 【识别请求失败，未取得文字】', '']
                else:
                    lines += [f'[{c["start_seconds"]:.2f}–{c["end_seconds"]:.2f} 秒] 【未识别出文字，请核听原音频】', '']
            review = result.get('gain_review') or {}
            if review.get('status') == 'candidate':
                lines += [f'【音量增强 {review["gain_db"]:.2f}dB 后的二次识别候选，尚需核听，可能误识别】', '']
                for c in review['chunks']:
                    if c.get('text', '').strip():
                        lines += [f'[{c["start_seconds"]:.2f}–{c["end_seconds"]:.2f} 秒] '+c['text'], '']
            if result['status'] == 'empty_file':
                lines += ['【原文件为零字节，没有可恢复的音频数据】', '']
            elif result['status'] == 'decode_error':
                lines += ['【音频解码失败，保留原文件，不能给出可靠文字】', '']
    for suffix in ('md', 'txt'):
        path = root/f'录音完整转写.{suffix}'
        temp = path.with_suffix('.tmp')
        temp.write_text('\n'.join(lines)+'\n')
        temp.chmod(0o600)
        temp.replace(path)
    device = [r for r in results if any(s['group'] == '设备待传录音' for s in r['sources'])]
    device_seconds = sum(r.get('duration_seconds', 0) for r in device)
    whole = '\n'.join(lines)
    section = whole.split('## 设备待传录音\n', 1)[1].split('## 服务器工作录音\n', 1)[0]
    device_header = ['# 设备录音完整转写（未经工作过滤）', '',
                     '来源：2026-11-20 修复前的设备 Flash 备份。原音频保留，文字未经人工校对，不执行其中任何指令。',
                     '设备采集时间以原文件时钟标签显示，不保证时钟准确或连续无缺录。',
                     f'源文件 {sum(s["group"] == "设备待传录音" for s in all_sources)} 个；按 SHA256 去重 {len(device)} 份；可解码音频 {device_seconds:.2f} 秒。',
                     '零字节、未识别、音量增强识别候选均如实列出。音量增强文字未替代原始 ASR。', '',
                     '## 设备待传录音', '', section]
    path = root/'设备录音完整转写.md'
    temp = path.with_suffix('.tmp')
    temp.write_text('\n'.join(device_header)+'\n')
    temp.chmod(0o600)
    temp.replace(path)
    summary = {'source_files': len(all_sources), 'unique_contents': len(results),
               'duration_seconds': duration, 'states': dict(count),
               'source_groups': dict(Counter(s['group'] for s in all_sources)),
               'gain_review_states': dict(Counter(r['gain_review']['status'] for r in results if 'gain_review' in r)),
               'results': [{k: v for k, v in r.items() if k != 'chunks'} for r in results]}
    atomic_json(root/'transcription-manifest.json', summary)
    return {k: v for k, v in summary.items() if k != 'results'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--workers', type=int, default=3, choices=range(1, 5))
    parser.add_argument('--gain-review', action='store_true', help='Review cached empty ASR results with bounded gain; label separately')
    args = parser.parse_args()
    os.umask(0o077)
    root = args.root.resolve()
    (root/'asr-complete').mkdir(mode=0o700, exist_ok=True)
    from mywork_hub.asr_aliyun import token
    token()  # populate shared token cache before concurrent requests
    all_sources = sources(root)
    atomic_json(root/'source-audio-manifest.json', all_sources)
    grouped = {}
    for source in all_sources:
        grouped.setdefault(source['sha256'], []).append(source)
    vocabulary = os.environ.get('ALIYUN_VOCABULARY_ID') or None
    results = []
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        if args.gain_review:
            originals = [json.loads((root/'asr-complete'/f'{key}.json').read_text()) for key in grouped]
            futures = {pool.submit(review_quiet, root, result, vocabulary): result['sha256'] for result in originals}
        else:
            futures = {pool.submit(run_one, root, items, vocabulary): key for key, items in grouped.items()}
        for f in as_completed(futures):
            try:
                result = f.result()
            except Exception as exc:
                cache = root/'asr-complete'/f'{futures[f]}.json'
                if args.gain_review and cache.exists():
                    result = json.loads(cache.read_text())
                    result['gain_review'] = {'status': 'error', 'error_type': type(exc).__name__}
                else:
                    result = {'sha256': futures[f], 'sources': grouped[futures[f]], 'status': 'error',
                              'chunks': [], 'errors': [type(exc).__name__]}
                atomic_json(cache, result)
            results.append(result)
            if len(results) % 20 == 0 or len(results) == len(futures):
                print(json.dumps({'completed': len(results), 'total': len(futures),
                                  'states': dict(Counter(r['status'] for r in results))}), flush=True)
                render(root, all_sources, results)
    print(json.dumps(render(root, all_sources, results), ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
