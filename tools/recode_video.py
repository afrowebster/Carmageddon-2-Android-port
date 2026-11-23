#!/usr/bin/env python3
"""Recode the game's Smacker videos (.smk) into C2V, a simple format the runtime plays natively
(win32/smack.c), so no Smacker DLL is needed and every frame is always decoded.

C2V layout (little endian):
  "C2V1", u32 width, u32 height, u32 frames, u32 us_per_frame, u32 audio_rate, u32 audio_samples
  per frame: u32 flags (bit 0: palette follows), [768 bytes RGB palette], u32 n, n bytes zlib(frame XOR previous frame)
  audio: audio_samples x s16 mono PCM

Usage: recode_video.py in.smk out.c2v
"""
import sys, struct, zlib
import av
import numpy as np

src, dst = sys.argv[1], sys.argv[2]
hdr = open(src, 'rb').read(20)
assert hdr[:3] == b'SMK', src
rate = struct.unpack_from('<i', hdr, 16)[0]
us = rate * 1000 if rate > 0 else (-rate * 10 if rate < 0 else 100000)

c = av.open(src)
vs = c.streams.video[0]
w, h = vs.codec_context.width, vs.codec_context.height
frames = []
audio = []
arate = 0
prev = np.zeros(w * h, np.uint8)
prevpal = None
for pkt in c.demux():
    for fr in pkt.decode():
        if isinstance(fr, av.VideoFrame):
            idx = np.frombuffer(bytes(fr.planes[0]), np.uint8).reshape(h, fr.planes[0].line_size)[:, :w].reshape(-1)
            pal = np.frombuffer(bytes(fr.planes[1]), np.uint8).reshape(256, 4)[:, [2, 1, 0]].tobytes()  # BGRA -> RGB
            flags = 0
            out = b''
            if pal != prevpal:
                flags |= 1; out += pal; prevpal = pal
            z = zlib.compress((idx ^ prev).tobytes(), 9)
            frames.append(struct.pack('<I', flags) + out + struct.pack('<I', len(z)) + z)
            prev = idx.copy()
        elif isinstance(fr, av.AudioFrame):
            arate = fr.sample_rate
            a = fr.to_ndarray()
            if a.dtype == np.uint8: a = (a.astype(np.int16) - 128) << 8
            elif a.dtype != np.int16: a = (np.clip(a, -1, 1) * 32767).astype(np.int16)
            ch = len(fr.layout.channels)
            if a.ndim == 2 and a.shape[0] > 1: a = a.astype(np.int32).mean(axis=0)          # planar
            elif ch > 1: a = a.reshape(-1, ch).astype(np.int32).mean(axis=1)               # packed
            audio.append(np.asarray(a, np.int16).reshape(-1).astype('<i2'))
pcm = np.concatenate(audio).tobytes() if audio else b''
with open(dst, 'wb') as f:
    f.write(b'C2V1' + struct.pack('<6I', w, h, len(frames), us, arate, len(pcm) // 2))
    for fr in frames: f.write(fr)
    f.write(pcm)
print('%s: %dx%d, %d frames @ %.2f fps, audio %d Hz %.1fs -> %s (%d KB)' % (
    src, w, h, len(frames), 1e6 / us, arate, len(pcm) / 2 / max(arate, 1), dst, (24 + sum(map(len, frames)) + len(pcm)) // 1024))
