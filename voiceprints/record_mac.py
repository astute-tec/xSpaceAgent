#!/usr/bin/env python3
"""用 Mac 麦克风录声纹"""
import sys, time, json
import numpy as np
import sounddevice as sd
from pathlib import Path
from resemblyzer import VoiceEncoder, preprocess_wav

OUT = Path(__file__).resolve().parents[1] / "voiceprints" / "audio"
OUT.mkdir(parents=True, exist_ok=True)
SAMPLE_RATE = 16000

def record(seconds: float, hint: str) -> np.ndarray:
    print(f"  请说: {hint}")
    print(f"  ({seconds} 秒后自动停止)")
    audio = sd.rec(int(seconds * SAMPLE_RATE), samplerate=SAMPLE_RATE, channels=1, dtype='int16')
    sd.wait()
    return audio.flatten().astype(np.float32) / 32768.0

def main():
    names = sys.argv[1:]
    if not names:
        print("用法: record_mac.py 名字1 [名字2 ...]")
        return
    
    encoder = VoiceEncoder()
    lib_file = Path(__file__).resolve().parents[1] / "voiceprints" / "library.json"
    lib = json.loads(lib_file.read_text()) if lib_file.exists() else {}
    
    for name in names:
        print(f"\n=== {name} 声纹录入 ===")
        embeds = []
        for i in range(3):
            hint = [f"我是{name}", "今天我们在讨论项目的进展情况", f"我是{name}，负责这个项目"][i]
            audio = record(5.0, hint)
            wav = preprocess_wav(audio, source_sr=SAMPLE_RATE)
            emb = encoder.embed_utterance(wav)
            embeds.append(emb)
            print(f"  段{i+1}: 声纹向量维度 {len(emb)}")
        
        avg = np.mean(embeds, axis=0)
        avg = avg / np.linalg.norm(avg)
        lib[name] = {"embedding": avg.tolist(), "samples": len(embeds)}
        print(f"  ✓ {name} 已注册")
    
    lib_file.parent.mkdir(parents=True, exist_ok=True)
    lib_file.write_text(json.dumps(lib, ensure_ascii=False, indent=2))
    print(f"\n声纹库已保存: {', '.join(lib.keys())}")

if __name__ == "__main__":
    main()
