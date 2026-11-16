"""Software 'microphone' for the Nucleo: audio -> 1-bit PDM -> C header,
plus bit-exact Python references for the CIC and FIR."""
import numpy as np
from math import gcd
from scipy import signal
from scipy.io import wavfile

PDM_RATE = 1_250_000
CIC_R = 20                 # 1.25 MHz -> 62.5 kHz
FIR_R = 4                  # 62.5 kHz -> 15.625 kHz
CIC_SHIFT = 3              # CIC gain 20^4 = 160,000 (17.3 bits) -> divide by 8

def tone(freq, dur=0.2, amp=0.5):
    t = np.arange(int(PDM_RATE * dur)) / PDM_RATE
    return amp * np.sin(2 * np.pi * freq * t)

def wav_to_1p25M(path, max_dur=1.0, amp=0.5):
    sr, x = wavfile.read(path)
    x = x.astype(np.float64)
    if x.ndim > 1:
        x = x.mean(axis=1)
    x /= np.max(np.abs(x)) + 1e-9
    env = np.convolve(np.abs(x), np.ones(int(sr * 0.01)) / int(sr * 0.01), "same")
    start = max(0, int(np.argmax(env > 0.1)) - int(sr * 0.05))   # skip leading silence
    x = x[start: start + int(sr * max_dur)]
    g = gcd(PDM_RATE, sr)
    return amp * signal.resample_poly(x, PDM_RATE // g, sr // g)

def sigma_delta(x):
    """2nd-order, 1-bit. Stable for |x| <~ 0.6. Returns uint8 array of 0/1."""
    out = np.empty(len(x), dtype=np.uint8)
    i1 = i2 = 0.0
    y = 1.0
    for n, v in enumerate(x.tolist()):
        i1 += v - y
        i2 += i1 - y
        y = 1.0 if i2 >= 0.0 else -1.0
        out[n] = y > 0
    return out

def to_header(bits, name, path):
    bits = bits[: len(bits) // 8 * 8]
    packed = np.packbits(bits)                  # MSB first = first bit in time
    with open(path, "w") as f:
        f.write(f"// {len(bits)} PDM bits, {len(packed)} bytes, MSB first\n")
        f.write(f"#define {name.upper()}_BYTES {len(packed)}\n")
        f.write(f"static const uint8_t {name}[{len(packed)}] = {{\n")
        for i in range(0, len(packed), 24):
            f.write(",".join(str(v) for v in packed[i:i + 24]) + ",\n")
        f.write("};\n")
    return packed

def cic_ref(bits):
    """Bit-exact model of the C CIC (4th order, /20, uint32 wraparound)."""
    x = np.where(bits == 1, 1, -1).astype(np.int64).astype(np.uint32)
    for _ in range(4):
        x = np.cumsum(x, dtype=np.uint32)
    y = x[CIC_R - 1::CIC_R]
    for _ in range(4):
        y = np.concatenate((np.zeros(1, np.uint32), y))
        y = (y[1:] - y[:-1]).astype(np.uint32)
    return y.view(np.int32)

def fir_taps_q15(beta=4.0):
    h = signal.firwin(48, (6000 + 9600) / 2, window=("kaiser", beta), fs=62500)
    return np.round(h * 32768).astype(np.int32)      # sums to ~32768 (unity DC gain)

def fir_ref(cic_out, taps_q15):
    """Integer FIR + decimate by 4, same shift/rounding as the planned C."""
    s = (cic_out >> CIC_SHIFT).astype(np.int64)          # arithmetic shift
    y = np.convolve(s, taps_q15.astype(np.int64))[: len(s)]   # causal
    y = (y + (1 << 14)) >> 15                            # round, back to Q0
    return np.clip(y[::FIR_R], -32768, 32767).astype(np.int16)


if __name__ == "__main__":
    import argparse, os
    ap = argparse.ArgumentParser(description="make a simulated-mic header for the Nucleo")
    ap.add_argument("--tone", type=float, help="tone frequency in Hz")
    ap.add_argument("--wav", help="WAV file (mono or stereo, any rate)")
    ap.add_argument("--dur", type=float, default=0.2, help="seconds (flash cost 156 kB per second)")
    ap.add_argument("--out", default="fw", help="output folder")
    a = ap.parse_args()
    x = tone(a.tone, a.dur) if a.tone else wav_to_1p25M(a.wav, a.dur)
    bits = sigma_delta(x)
    os.makedirs(a.out, exist_ok=True)
    packed = to_header(bits, "pdm_test", os.path.join(a.out, "pdm_test.h"))
    taps = fir_taps_q15()
    with open(os.path.join(a.out, "fir_taps.h"), "w") as f:
        f.write("static const int16_t fir_taps[48] = {" + ",".join(map(str, taps)) + "};\n")
    c = cic_ref(bits)
    y = fir_ref(c, taps)
    print(f"flash used by the array: {len(packed)} bytes ({100 * len(packed) / 262144:.0f}% of 256 KB)")
    print(f"expected CIC outputs: {len(c)}, sum = {int(c.astype(np.int64).sum())}")
    print(f"expected FIR outputs: {len(y)}, sum = {int(y.astype(np.int64).sum())}")
    print("first 8 CIC:", c[:8].tolist())
    print("first 8 FIR:", y[:8].tolist())