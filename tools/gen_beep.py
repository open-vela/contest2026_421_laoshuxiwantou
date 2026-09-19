#!/usr/bin/env python3
"""生成演示用提示音 /data/beep.wav（16k/16bit/mono，0.5s 880Hz，淡入淡出）。

用法：python3 tools/gen_beep.py [输出路径]
把生成的 beep.wav 拷到 SD 卡 /data/ 下即可（板端 sm_beep_play 读取）。
"""
import math
import struct
import sys
import wave

SR = 16000
DUR = 0.5
FREQ = 880.0

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "tools/beep.wav"
    n = int(SR * DUR)
    frames = bytearray()
    for i in range(n):
        t = i / SR
        env = min(1.0, i / (0.02 * SR), (n - i) / (0.05 * SR))
        v = int(24000 * env * math.sin(2 * math.pi * FREQ * t))
        frames += struct.pack("<h", v)
    with wave.open(out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(bytes(frames))
    print("wrote", out, len(frames), "bytes")

if __name__ == "__main__":
    main()
