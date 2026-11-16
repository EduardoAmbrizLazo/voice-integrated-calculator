"""Software I2S microphone for the Nucleo (Option B).

Models the SPH0645LM4H running at 32 kHz: 18-bit samples left-justified in
32-bit words (sample << 14). Firmware unpacks them, decimates /2 with a
31-tap half-band FIR to 16 kHz, removes DC, runs a 12-band filter bank, and
makes one 12-byte feature frame per 256 output samples (16 ms).
This file writes the C headers and the expected values for that firmware.

  python pcm_sim.py --tone 1000 --out fw           # test tone
  python pcm_sim.py --wav seven.wav --dur 0.5 --out fw
  python pcm_sim.py --demo                          # 11 kHz aliasing demo (FIR on vs bypass)
"""
import argparse, os
from math import gcd
import numpy as np
from scipy import signal
from scipy.io import wavfile

FS_MIC, FS_OUT = 32000, 16000
FRAME_OUT = 256                      # output samples per frame (16 ms)
FRAME_IN = 2 * FRAME_OUT             # mic words per frame (512)
NB, F_LO, F_HI = 12, 200.0, 6000.0
DC_POLE = 0.995

# ---------------------------------------------------------------- mic model
def tone(freq, dur, peak_dbfs):
    t = np.arange(int(FS_MIC * dur)) / FS_MIC
    return 10 ** (peak_dbfs / 20) * np.sin(2 * np.pi * freq * t)

def wav_to_mic(path, dur, peak_dbfs):
    sr, x = wavfile.read(path)
    x = x.astype(np.float64)
    if x.ndim > 1:
        x = x.mean(axis=1)
    x /= np.max(np.abs(x)) + 1e-9
    env = np.convolve(np.abs(x), np.ones(int(sr * 0.01)) / int(sr * 0.01), "same")
    start = max(0, int(np.argmax(env > 0.1)) - int(sr * 0.05))      # skip leading silence
    x = x[start: start + int(sr * dur)]
    g = gcd(FS_MIC, sr)
    return 10 ** (peak_dbfs / 20) * signal.resample_poly(x, FS_MIC // g, sr // g)

def to_words(x):
    """18-bit two's complement, left-justified in a 32-bit word, like the mic."""
    s18 = np.clip(np.round(x * 2 ** 17), -2 ** 17, 2 ** 17 - 1).astype(np.int64)
    n = len(s18) // FRAME_IN * FRAME_IN                              # whole frames only
    return (s18[:n] << 14).astype(np.int32)

# ------------------------------------------------------------- filters
def halfband_taps(n=31, beta=4.0):
    h = signal.firwin(n, FS_MIC / 4, window=("kaiser", beta), fs=FS_MIC)
    return np.round(h * 32768).astype(np.int32)          # Q15, zeros at even offsets

def bank_sos():
    r = (F_HI / F_LO) ** (1.0 / NB)
    q = 1.0 / (np.sqrt(r) - 1.0 / np.sqrt(r))
    sos = []
    for fc in F_LO * r ** (np.arange(NB) + 0.5):
        b, a = signal.iirpeak(fc, q, fs=FS_OUT)
        sos.append(signal.tf2sos(b, a)[0])
    return np.array(sos)                                  # rows: b0 b1 b2 a0 a1 a2

# ---------------------------------------------------- reference chain
def unpack(words):
    return (words >> 16).astype(np.int64)                 # top 16 of the 18 bits

def fir_decim2(x16, taps, bypass=False):
    if bypass:
        return x16[::2].astype(np.int32)
    y = np.convolve(x16, taps.astype(np.int64))[: len(x16)]
    return ((y + (1 << 14)) >> 15)[::2].astype(np.int32)

def features(y16):
    x = signal.lfilter([1.0, -1.0], [1.0, -DC_POLE], y16 / 32768.0)
    n = len(x) // FRAME_OUT
    x = x[: n * FRAME_OUT]
    q = np.empty((n, NB), np.uint8)
    for k, s in enumerate(bank_sos()):
        e = (signal.sosfilt(s[None, :], x).reshape(n, FRAME_OUT) ** 2).mean(axis=1)
        db = 10 * np.log10(e + 1e-12)
        q[:, k] = np.clip(np.round((db + 100) * 2.55), 0, 255)
    return q

# --------------------------------------------------------- output files
def write_headers(words, out):
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "i2s_test.h"), "w") as f:
        f.write(f"// {len(words)} mic words (18-bit sample << 14), 32 kHz\n")
        f.write(f"#define I2S_TEST_WORDS {len(words)}\n")
        f.write(f"static const int32_t i2s_test[{len(words)}] = {{\n")
        for i in range(0, len(words), 12):
            f.write(",".join(str(int(v)) for v in words[i:i + 12]) + ",\n")
        f.write("};\n")
    taps = halfband_taps()
    with open(os.path.join(out, "fir_taps.h"), "w") as f:
        f.write("#define FIR_NT 31\n")
        f.write("static const int16_t fir_taps[31] = {" + ",".join(map(str, taps)) + "};\n")
    sos = bank_sos()
    with open(os.path.join(out, "bank.h"), "w") as f:
        f.write("// rows: b0, b1, b2, a1, a2  (a0 = 1)\nstatic const float bank_sos[12][5] = {\n")
        for r in sos:
            f.write("  {" + ",".join(f"{v:.9e}f" for v in (r[0], r[1], r[2], r[4], r[5])) + "},\n")
        f.write("};\n")

def strongest(y, fs):
    y = y[16:].astype(float)
    X = np.abs(np.fft.rfft(y * np.hanning(len(y)))) / (len(y) / 4)
    i = int(np.argmax(X))
    return np.fft.rfftfreq(len(y), 1 / fs)[i], 20 * np.log10(X[i] + 1e-9)

def demo():
    words = to_words(tone(11000, 0.5, -20))
    x16 = unpack(words)
    taps = halfband_taps()
    w, H = signal.freqz(taps / 32768.0, worN=8192, fs=FS_MIC)
    mag = 20 * np.log10(abs(H) + 1e-12)
    print(f"half-band FIR (31 taps, Q15): worst stopband >= 10 kHz = {mag[w >= 10000].max():.1f} dB, "
          f"passband 0-6 kHz within {mag[w <= 6000].min():.2f}..{mag[w <= 6000].max():.2f} dB")
    for label, byp in (("FIR on    ", False), ("FIR bypass", True)):
        f, lvl = strongest(fir_decim2(x16, taps, byp), FS_OUT)
        print(f"11 kHz tone, {label}: strongest component {f:6.0f} Hz at {lvl:6.1f} dB")

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--tone", type=float)
    ap.add_argument("--wav")
    ap.add_argument("--dur", type=float, default=0.5, help="seconds (flash cost 128 kB per second)")
    ap.add_argument("--peak", type=float, default=-30.0, help="peak level in dBFS")
    ap.add_argument("--out", default="fw")
    ap.add_argument("--demo", action="store_true")
    a = ap.parse_args()
    if a.demo:
        demo(); raise SystemExit
    x = tone(a.tone, a.dur, a.peak) if a.tone else wav_to_mic(a.wav, a.dur, a.peak)
    words = to_words(x)
    write_headers(words, a.out)
    taps = halfband_taps()
    y16 = fir_decim2(unpack(words), taps)
    q = features(y16)
    print(f"flash used by the array: {words.nbytes} bytes ({100 * words.nbytes / 262144:.0f}% of 256 KB)")
    print(f"frames: {len(q)}  (each = {FRAME_IN} mic words = 16 ms)")
    print(f"expected FIR outputs: {len(y16)}, sum = {int(y16.astype(np.int64).sum())}")
    print("first 8 FIR:", y16[:8].tolist())
    print(f"expected feature sum (all frames) = {int(q.astype(np.int64).sum())}  (C may differ by a few units)")
    print("first feature frame:", q[0].tolist())
