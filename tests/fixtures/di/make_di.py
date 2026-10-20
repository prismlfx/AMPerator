# Synthesizes a 5 s mono 48 kHz 16-bit "DI-like" guitar clip: Karplus-Strong strummed chords.
# Peak matches NAM's loudness reference input (nam/models/_resources/loudness_input.wav: peak -3 dBFS,
# RMS -15 dBFS); the clip's own RMS measures -19.4 dBFS over its 5 s (strum decays), so a model's "loudness"
# metadata only roughly predicts its output level for this clip.
import random, struct, wave, sys, math
sr = 48000; total = 5 * sr
random.seed(6)
out = [0.0] * total
E2, A2, D3, G3, B3, E4 = 82.41, 110.0, 146.83, 196.0, 246.94, 329.63
chords = [
    (0.0, [E2, 123.47, 164.81, 207.65, B3, E4]),   # E major
    (1.25, [A2, 164.81, 220.0, 277.18, E4]),       # A major
    (2.5, [D3, A2 * 2, 293.66, 369.99]),           # D major
    (3.75, [E2, 123.47, 164.81, 207.65, B3, E4]),  # E major
]
for start, freqs in chords:
    for s, f in enumerate(freqs):
        n = int(sr / f)
        buf = [random.uniform(-1, 1) for _ in range(n)]
        for _ in range(4):  # soft (finger/thumb) pluck: lowpass the excitation
            buf = [(buf[i] + buf[i - 1]) * 0.5 for i in range(n)]
        s0 = int((start + s * 0.012) * sr)  # 12 ms strum spread
        idx = 0
        for t in range(s0, total):
            v = buf[idx]
            buf[idx] = 0.998 * 0.5 * (v + buf[(idx + 1) % n])
            out[t] += v
            idx = (idx + 1) % n
for i in range(2400):  # short fade-out
    out[total - 1 - i] *= i / 2400
g = (10 ** (-3 / 20)) / max(abs(x) for x in out)
w = wave.open(sys.argv[1], 'wb')
w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b''.join(struct.pack('<h', int(round(x * g * 32767))) for x in out))
w.close()
rms = math.sqrt(sum((x * g) ** 2 for x in out) / total)
print('peak -3 dBFS, rms dBFS %.2f' % (20 * math.log10(rms)))  # ~ -19.4
