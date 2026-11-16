#!/usr/bin/env python3
"""
dtw_poc.py - Python model of the MCU speech recognizer (E155 voice calculator)

It mirrors the planned MCU algorithm, so the numbers it produces are the ones
the C code should reproduce:

  audio -> 16 kHz -> DC block -> 12 constant-Q band-pass biquads
        -> 256-sample frames (16 ms) -> log energy per band, 8-bit
        -> energy endpointing (3 loud frames start, 8 quiet frames end,
           3-frame pre-roll, 5..50 frames)
        -> subtract the word's mean per band
        -> DTW (Sakoe-Chiba band +/-8 around the length-scaled diagonal,
           L1 frame distance, normalized by path length) vs enrolled templates
        -> lowest distance wins; reject if above a threshold

USAGE
  python dtw_poc.py                      synthetic proof of concept (no data needed)
  python dtw_poc.py --sweep              also run accuracy-vs-noise sweep
  python dtw_poc.py --data data/         real recordings: data/<speaker>/<word>_<k>.wav
  python dtw_poc.py --split session.wav --speaker cheyenne --out data/ \
        --order "zero one two three four five six seven eight nine plus minus multiply equals clear clock timer start stop"
        (cuts one long recording of the words spoken in order, repeated, into files)

The synthetic mode proves the CODE (features, endpointing, DTW, scoring, plots).
It does NOT prove speech accuracy: only real recordings can do that.
Needs: numpy, scipy, (matplotlib for the figure).
"""
import argparse
import os
import re
import sys
import time

import numpy as np
from scipy import signal
from scipy.io import wavfile

# ----------------------------------------------------------------------------
# Parameters that match the proposal
# ----------------------------------------------------------------------------
FS = 16000                      # audio rate after the FIR /2 (mic runs at 32 kHz)
FRAME = 256                     # samples per feature frame (16 ms)
NB = 12                         # number of bands
F_LO, F_HI = 200.0, 6000.0      # band-bank span
WORDS = ("zero one two three four five six seven eight nine "
         "plus minus multiply equals clear clock timer start stop").split()

CFG = dict(
    band=8,            # Sakoe-Chiba half-width, frames
    loud=3,            # consecutive loud frames that start a word
    quiet=8,           # consecutive quiet frames that end a word (128 ms)
    preroll=3,         # frames kept before the start
    min_frames=5,
    max_frames=50,
    margin_db=10.0,    # loud = total energy > noise floor + margin
    floor_db=-65.0,    # never treat anything below this as speech
)


# ----------------------------------------------------------------------------
# Feature extraction
# ----------------------------------------------------------------------------
def make_filterbank():
    """12 log-spaced constant-Q band-pass biquads covering 200-6000 Hz."""
    r = (F_HI / F_LO) ** (1.0 / NB)                  # edge ratio, 1.328
    q = 1.0 / (np.sqrt(r) - 1.0 / np.sqrt(r))        # Q, about 3.5
    centers = F_LO * r ** (np.arange(NB) + 0.5)
    sos = []
    for fc in centers:
        b, a = signal.iirpeak(fc, q, fs=FS)          # one biquad per band
        sos.append(signal.tf2sos(b, a))
    return centers, q, sos


CENTERS, BAND_Q, SOS = make_filterbank()


def load_audio(path):
    """Read a WAV, mix to mono, resample to 16 kHz, scale to +/-1."""
    sr, x = wavfile.read(path)
    if x.ndim > 1:
        x = x.mean(axis=1)
    if x.dtype == np.int16:
        x = x / 32768.0
    elif x.dtype == np.int32:
        x = x / 2147483648.0
    elif x.dtype == np.uint8:
        x = (x.astype(np.float64) - 128.0) / 128.0
    else:
        x = x.astype(np.float64)
    if sr != FS:
        g = np.gcd(int(sr), FS)
        x = signal.resample_poly(x, FS // g, int(sr) // g)
    return x


def band_features(x):
    """Return (q, total_db): q is (frames, 12) uint8 log energy, total_db is
    the whole-signal level per frame (used only for endpointing)."""
    x = signal.lfilter([1.0, -1.0], [1.0, -0.995], x)      # DC blocker
    n = len(x) // FRAME
    x = x[: n * FRAME]
    db = np.empty((n, NB))
    for k, s in enumerate(SOS):
        y = signal.sosfilt(s, x)
        e = (y.reshape(n, FRAME) ** 2).mean(axis=1)
        db[:, k] = 10.0 * np.log10(e + 1e-12)
    q = np.clip(np.round((db + 100.0) * 2.55), 0, 255).astype(np.uint8)   # 8 bits
    total = 10.0 * np.log10((x.reshape(n, FRAME) ** 2).mean(axis=1) + 1e-12)
    return q, total


def find_segments(total_db, cfg=CFG):
    """Energy endpointer. Returns a list of (start, end) frame indices."""
    floor = np.percentile(total_db, 10)
    thr = max(floor + cfg["margin_db"], cfg["floor_db"])
    loud = total_db > thr
    n, segs, i = len(loud), [], 0
    while i < n:
        if i + cfg["loud"] <= n and loud[i:i + cfg["loud"]].all():
            start = max(0, i - cfg["preroll"])
            j, quiet, last = i + cfg["loud"], 0, i + cfg["loud"] - 1
            while j < n:
                if loud[j]:
                    quiet, last = 0, j
                else:
                    quiet += 1
                    if quiet >= cfg["quiet"]:
                        break
                j += 1
            segs.append((start, last + 1))
            i = j + 1
        else:
            i += 1
    return segs


def extract_word(q, total_db, cfg=CFG):
    """Features of the (longest) word in a clip, mean-subtracted, in dB units.
    Returns None if no word is found."""
    segs = [s for s in find_segments(total_db, cfg) if s[1] - s[0] >= cfg["min_frames"]]
    if not segs:
        return None
    s, e = max(segs, key=lambda se: se[1] - se[0])
    f = q[s:e][: cfg["max_frames"]].astype(np.float32) / 2.55    # back to dB
    return f - f.mean(axis=0, keepdims=True)                     # per-band mean


def clip_to_features(x):
    q, total = band_features(x)
    return extract_word(q, total)


# ----------------------------------------------------------------------------
# DTW (plain loops on purpose: this maps line-for-line onto the C code)
# ----------------------------------------------------------------------------
def _dtw_cost(D, band):
    """Banded symmetric DTW. Steps cost d (vertical, horizontal) or 2d
    (diagonal), so the path weight always totals n+m."""
    n, m = D.shape
    INF = 1e30
    Dl = D.tolist()
    C = [[INF] * m for _ in range(n)]
    for i in range(n):
        c = i * (m - 1) / (n - 1) if n > 1 else 0.0
        jlo = max(0, int(np.ceil(c - band)))
        jhi = min(m - 1, int(np.floor(c + band)))
        Ci = C[i]
        Cp = C[i - 1] if i > 0 else None
        Di = Dl[i]
        for j in range(jlo, jhi + 1):
            d = Di[j]
            if i == 0 and j == 0:
                Ci[j] = 2.0 * d
                continue
            best = INF
            if i > 0:
                v = Cp[j] + d
                if v < best:
                    best = v
                if j > 0:
                    v = Cp[j - 1] + 2.0 * d
                    if v < best:
                        best = v
            if j > 0:
                v = Ci[j - 1] + d
                if v < best:
                    best = v
            Ci[j] = best
    return C


def _frame_distances(a, b):
    return np.abs(a[:, None, :] - b[None, :, :]).sum(axis=2).astype(np.float64)


def dtw_distance(a, b, band=CFG["band"]):
    """Mean |difference| per band per step, in dB (path-length normalized)."""
    n, m = len(a), len(b)
    if n > 2 * m or m > 2 * n:             # lengths too different: no match
        return np.inf
    C = _dtw_cost(_frame_distances(a, b), band)
    return C[n - 1][m - 1] / (n + m) / a.shape[1]


def dtw_path(a, b, band=CFG["band"]):
    """Same DP plus a backtrack, for the figure."""
    D = _frame_distances(a, b)
    C = np.array(_dtw_cost(D, band))
    i, j = len(a) - 1, len(b) - 1
    path = [(i, j)]
    while i > 0 or j > 0:
        d = D[i, j]
        cands = []
        if i > 0 and j > 0:
            cands.append((C[i - 1, j - 1] + 2 * d, i - 1, j - 1))
        if i > 0:
            cands.append((C[i - 1, j] + d, i - 1, j))
        if j > 0:
            cands.append((C[i, j - 1] + d, i, j - 1))
        _, i, j = min(cands)
        path.append((i, j))
    C[C > 1e29] = np.nan
    return path[::-1], C


# ----------------------------------------------------------------------------
# Recognizer
# ----------------------------------------------------------------------------
def classify(feat, templates, band=CFG["band"]):
    best_d, best_w = np.inf, None
    for w, t in templates:
        d = dtw_distance(feat, t, band)
        if d < best_d:
            best_d, best_w = d, w
    return best_w, best_d


def loo_threshold(templates, band, k):
    """Rejection threshold from the enrollment set alone: for each template,
    the distance to its nearest same-word template; threshold = mean + k*std."""
    ds = []
    for i, (w, t) in enumerate(templates):
        same = [dtw_distance(t, u, band) for j, (w2, u) in enumerate(templates)
                if j != i and w2 == w]
        if same:                       # words with <2 usable templates are skipped
            ds.append(min(same))
    if not ds:
        return 0.0                     # nothing usable: everything gets rejected
    ds = np.array(ds)
    return float(ds.mean() + k * ds.std())


def evaluate(ds_enroll, ds_test, n_enroll=3, band=CFG["band"], k_thr=3.0, oov=None):
    """Speaker-dependent test: per speaker, the first n_enroll recordings of
    each word are the templates and the remaining recordings are tested."""
    idx = {w: i for i, w in enumerate(WORDS)}
    conf = np.zeros((len(WORDS), len(WORDS) + 1), int)   # last column = rejected
    oov_n = oov_rej = 0
    thr_used = []
    for spk in ds_enroll:
        tmpl = [(w, f) for w in WORDS for f in ds_enroll[spk][w][:n_enroll] if f is not None]
        thr = loo_threshold(tmpl, band, k_thr)
        thr_used.append(thr)
        for w in WORDS:
            for f in ds_test[spk][w][n_enroll:]:
                if f is None:
                    conf[idx[w], -1] += 1
                    continue
                pred, dist = classify(f, tmpl, band)
                conf[idx[w], -1 if dist > thr else idx[pred]] += 1
        for f in (oov or {}).get(spk, []):
            oov_n += 1
            if f is None or classify(f, tmpl, band)[1] > thr:
                oov_rej += 1
    acc = np.trace(conf[:, :-1]) / max(conf.sum(), 1)
    return dict(acc=acc, conf=conf, thr=float(np.mean(thr_used)),
                false_reject=conf[:, -1].sum() / max(conf.sum(), 1),
                oov_reject=(oov_rej / oov_n) if oov_n else None)


def top_confusions(conf, n=5):
    pairs = []
    for i, w in enumerate(WORDS):
        for j, v in enumerate(WORDS):
            if i != j and conf[i, j]:
                pairs.append((conf[i, j], w, v))
    return sorted(pairs, reverse=True)[:n]


# ----------------------------------------------------------------------------
# Synthetic "speech" (so the pipeline can be tested before any recordings)
# ----------------------------------------------------------------------------
def random_word_model(rng):
    segs = []
    for _ in range(rng.integers(3, 7)):
        segs.append(dict(dur=rng.uniform(90, 230),
                         F=np.array([rng.uniform(300, 850), rng.uniform(850, 2400),
                                     rng.uniform(2400, 3300)]),
                         fric=bool(rng.random() < 0.2), fc=rng.uniform(3000, 5500)))
    return segs


def make_vocab(seed=1234):
    rng = np.random.default_rng(seed)
    vocab = {w: random_word_model(rng) for w in WORDS}
    for a, b in SIMILAR_PAIRS:           # deliberately hard: one segment differs
        segs = [dict(s, F=s["F"].copy()) for s in vocab[a]]
        k = len(segs) // 2
        segs[k]["F"] = segs[k]["F"] * rng.uniform(1.12, 1.25)
        vocab[b] = segs
    return vocab


SIMILAR_PAIRS = [("five", "nine"), ("start", "stop"), ("plus", "minus"), ("timer", "clock")]
SPEAKERS = {"spk_A": dict(vt=0.92, f0=210.0),
            "spk_B": dict(vt=1.00, f0=140.0),
            "spk_C": dict(vt=1.10, f0=105.0)}
_PINK_B = [0.049922035, -0.095993537, 0.050612699, -0.004408786]
_PINK_A = [1.0, -2.494956002, 2.017265875, -0.522189400]


def synth_utterance(segs, spk, rng, snr_db):
    """Voiced formant-like source plus fricative noise, with random speaking
    rate, per-segment timing jitter, level, and pink background noise."""
    rate = rng.uniform(0.85, 1.15)
    durs = np.array([s["dur"] * rate * rng.uniform(0.85, 1.15) for s in segs]) * FS / 1000.0
    edges = np.concatenate(([0], np.cumsum(durs))).astype(int)
    T = int(edges[-1])
    t = np.arange(T)
    knots = (edges[:-1] + edges[1:]) / 2.0
    vt = spk["vt"] * rng.uniform(0.97, 1.03)
    F = np.array([s["F"] for s in segs]) * vt
    Ft = [np.interp(t, knots, F[:, k]) for k in range(3)]
    voiced = np.interp(t, knots, [0.2 if s["fric"] else 1.0 for s in segs])
    f0 = spk["f0"] * rng.uniform(0.95, 1.05) * (1.08 - 0.16 * t / T)
    phase = 2 * np.pi * np.cumsum(f0) / FS
    bw, gain = (90.0, 140.0, 200.0), (1.0, 0.7, 0.4)
    x = np.zeros(T)
    for h in range(1, int(0.45 * FS / f0.min()) + 1):
        fh = h * f0
        env = sum(gain[k] * np.exp(-0.5 * ((fh - Ft[k]) / bw[k]) ** 2) for k in range(3))
        x += env * np.sin(h * phase) / h ** 0.7
    x *= voiced
    for s, e0, e1 in zip(segs, edges[:-1], edges[1:]):
        if s["fric"]:
            fc = s["fc"] * vt
            sos = signal.butter(2, [0.8 * fc, min(1.25 * fc, 0.95 * FS / 2)],
                                btype="band", fs=FS, output="sos")
            nz = signal.sosfilt(sos, rng.standard_normal(e1 - e0)) * np.hanning(e1 - e0)
            x[e0:e1] += 1.5 * nz
    ramp = int(0.015 * FS)
    w = np.ones(T)
    w[:ramp], w[-ramp:] = np.linspace(0, 1, ramp), np.linspace(1, 0, ramp)
    x *= w
    x *= 0.1 * 10 ** (rng.uniform(-3, 3) / 20) / (np.sqrt(np.mean(x ** 2)) + 1e-12)
    speech_rms = np.sqrt(np.mean(x ** 2))
    pre, post = int(rng.uniform(0.25, 0.5) * FS), int(rng.uniform(0.3, 0.5) * FS)
    out = np.zeros(pre + T + post)
    out[pre:pre + T] = x
    noise = signal.lfilter(_PINK_B, _PINK_A, rng.standard_normal(len(out)))
    noise *= speech_rms * 10 ** (-snr_db / 20) / (noise.std() + 1e-12)
    return out + noise + rng.standard_normal(len(out)) * 10 ** (-72 / 20)


def make_synth(vocab, reps, snr_db, seed):
    rng = np.random.default_rng(seed)
    return {spk: {w: [clip_to_features(synth_utterance(vocab[w], p, rng, snr_db))
                      for _ in range(reps)] for w in WORDS}
            for spk, p in SPEAKERS.items()}


def make_oov(n_per_speaker, snr_db, seed):
    """Words that are NOT in the vocabulary (should be rejected)."""
    rng = np.random.default_rng(seed)
    return {spk: [clip_to_features(synth_utterance(random_word_model(rng), p, rng, snr_db))
                  for _ in range(n_per_speaker)] for spk, p in SPEAKERS.items()}


# ----------------------------------------------------------------------------
# Real recordings
# ----------------------------------------------------------------------------
def load_dataset(path):
    """data/<speaker>/<word>_<k>.wav   (or <word>_<k>.wav directly in path)"""
    ds = {}
    spk_dirs = [d for d in sorted(os.listdir(path)) if os.path.isdir(os.path.join(path, d))]
    roots = [(d, os.path.join(path, d)) for d in spk_dirs] or [("speaker", path)]
    for spk, root in roots:
        files = {}
        for fn in sorted(os.listdir(root)):
            m = re.match(r"([a-z]+)_(\d+)\.wav$", fn.lower())
            if m and m.group(1) in WORDS:
                files.setdefault(m.group(1), []).append((int(m.group(2)), fn))
        if not files:
            continue
        ds[spk] = {}
        for w in WORDS:
            ds[spk][w] = [clip_to_features(load_audio(os.path.join(root, fn)))
                          for _, fn in sorted(files.get(w, []))]
    return ds


def split_session(path, speaker, order, outdir, cfg=CFG, reps=None, min_ms=120,
                  drop=(), dry_run=False, force=False):
    """Cut one long recording (words spoken in `order`, repeated, with pauses)
    into <word>_<k>.wav files. Detection runs on a 16 kHz copy, but the clips
    are cut from your ORIGINAL file, so sample rate and format are unchanged.
    Prints a table of what it found; names are only trustworthy when the number
    of detected words is exactly len(order) x repetitions."""
    sr, raw = wavfile.read(path)
    mono = raw.astype(np.float64)
    if mono.ndim > 1:
        mono = mono.mean(axis=1)
    g = np.gcd(int(sr), FS)
    x = signal.resample_poly(mono / (np.max(np.abs(mono)) + 1e-9), FS // g, int(sr) // g)
    q, total = band_features(x)
    min_frames = max(cfg["min_frames"], int(np.ceil(min_ms / 16.0)))
    segs = [s for s in find_segments(total, cfg) if s[1] - s[0] >= min_frames]
    words = order.split()
    drop = set(int(d) for d in drop)

    print(f"found {len(segs)} sounds (ignoring anything shorter than {min_frames * 16} ms)")
    kept = [s for i, s in enumerate(segs) if i not in drop]
    print("  #   start(s)  length(ms)  name if kept in order      note")
    for i, (st, en) in enumerate(segs):
        ms = (en - st) * 16
        if i in drop:
            name, note = "-", "dropped"
        else:
            k = kept.index((st, en)) if (st, en) in kept else 0
            name = f"{words[k % len(words)]}_{k // len(words)}"
            note = "SHORT, maybe noise" if ms < 200 else ("LONG, maybe two sounds" if ms > 1400 else "")
        print(f"{i:4d}  {st * 0.016:8.2f}  {ms:9d}   {name:24s} {note}")

    n = len(kept)
    expected = len(words) * reps if reps else None
    bad = (n % len(words) != 0) or (expected is not None and n != expected)
    if bad:
        print(f"\nNOT WRITING FILES: {n} sounds is not "
              f"{expected if expected else 'a multiple of ' + str(len(words))}. "
              "Names after the first extra or missing sound would be wrong.")
        print("Fix it by one of:\n"
              "  --drop 5,17      remove sounds by their # above (clicks, breaths, coughs)\n"
              "  --margin-db 15   ignore quieter sounds (default 10)\n"
              "  --min-ms 200     ignore shorter sounds (default 120)\n"
              "  --quiet-frames 12  join words that were split by a short gap (default 8)\n"
              "  --force          write anyway")
        if not force:
            return
    if dry_run:
        print("\n(dry run: nothing written)")
        return
    os.makedirs(os.path.join(outdir, speaker), exist_ok=True)
    spf = sr * FRAME / FS                                     # original samples per frame
    for i, (st, en) in enumerate(kept):
        lo = st - (25 if i == 0 else min(25, (st - kept[i - 1][1]) // 2))
        hi = en + (25 if i == n - 1 else min(25, (kept[i + 1][0] - en) // 2))
        lo, hi = max(0, lo), min(len(total), hi)
        clip = raw[int(lo * spf): int(hi * spf)]              # original samples, untouched
        name = f"{words[i % len(words)]}_{i // len(words)}.wav"
        wavfile.write(os.path.join(outdir, speaker, name), sr, clip)
    print(f"\nwrote {n} clips to {os.path.join(outdir, speaker)} at the original {sr} Hz")


# ----------------------------------------------------------------------------
# Figure
# ----------------------------------------------------------------------------
def make_figure(path, example, conf, sweep):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, ax = plt.subplots(2, 2, figsize=(14, 11))
    a, b = example
    ax[0, 0].imshow(a.T, aspect="auto", origin="lower", cmap="magma")
    ax[0, 0].set_title("12-band log-energy features of one word (mean-subtracted, dB)")
    ax[0, 0].set_xlabel("frame (16 ms each)")
    ax[0, 0].set_ylabel("band (low to high)")

    path_, C = dtw_path(a, b)
    ax[0, 1].imshow(C.T, origin="lower", aspect="auto", cmap="viridis")
    ax[0, 1].plot([p[0] for p in path_], [p[1] for p in path_], "w-", lw=2)
    ax[0, 1].set_title("DTW cost, 2 utterances of the same word\n(band +/-8 frames, warping path in white)")
    ax[0, 1].set_xlabel("utterance 1 frame")
    ax[0, 1].set_ylabel("utterance 2 frame")

    ax[1, 0].imshow(conf, cmap="Blues", aspect="auto")
    ax[1, 0].set_xticks(range(len(WORDS) + 1))
    ax[1, 0].set_xticklabels(list(WORDS) + ["REJECT"], rotation=90, fontsize=7)
    ax[1, 0].set_yticks(range(len(WORDS)))
    ax[1, 0].set_yticklabels(WORDS, fontsize=7)
    ax[1, 0].set_title("Confusion matrix (rows = spoken, columns = recognized)")

    if sweep:
        snr = [r[0] for r in sweep]
        ax[1, 1].plot(snr, [100 * r[1] for r in sweep], "o-", label="enroll quiet, test noisy")
        ax[1, 1].plot(snr, [100 * r[2] for r in sweep], "s-", label="enroll in same noise")
        ax[1, 1].axhline(90, color="r", ls="--", label="90% target")
        ax[1, 1].invert_xaxis()
        ax[1, 1].set_xlabel("SNR (dB)")
        ax[1, 1].set_ylabel("word accuracy (%)")
        ax[1, 1].set_title("Accuracy vs background noise")
        ax[1, 1].legend()
        ax[1, 1].grid(alpha=0.3)
    else:
        ax[1, 1].axis("off")
        ax[1, 1].text(0.5, 0.5, "run with --sweep for the noise plot", ha="center")
    fig.tight_layout()
    fig.savefig(path, dpi=110)
    print("figure:", path)


# ----------------------------------------------------------------------------
# Main
# ----------------------------------------------------------------------------
def selftest():
    rng = np.random.default_rng(0)
    a = rng.normal(0, 6, (30, NB)).astype(np.float32)
    assert dtw_distance(a, a) < 1e-9
    idx = np.clip((np.arange(39) * 29 / 38).astype(int), 0, 29)       # 1.3x slower copy
    stretched = a[idx]
    other = rng.normal(0, 6, (39, NB)).astype(np.float32)
    assert dtw_distance(a, stretched) < 0.5 * dtw_distance(a, other)
    print("self-check OK: identical = 0, stretched copy << unrelated sequence")


def report(res, title):
    print(f"\n{title}")
    print(f"  word accuracy        : {100 * res['acc']:.1f}%   (rejections count as errors)")
    print(f"  false reject rate    : {100 * res['false_reject']:.1f}%")
    if res["oov_reject"] is not None:
        print(f"  out-of-vocab rejected: {100 * res['oov_reject']:.1f}%")
    print(f"  threshold (mean)     : {res['thr']:.2f} dB per band per step")
    print(f"  4-word command       : {100 * res['acc'] ** 4:.1f}% (e.g. 'seven multiply six equals')")
    tc = top_confusions(res["conf"])
    if tc:
        print("  top confusions       : " + ", ".join(f"{a}->{b} x{n}" for n, a, b in tc))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", help="folder of real recordings")
    ap.add_argument("--split", help="long session WAV to cut into word files")
    ap.add_argument("--speaker", default="speaker")
    ap.add_argument("--order", default=" ".join(WORDS))
    ap.add_argument("--out", default="poc_out")
    ap.add_argument("--reps", type=int, default=8, help="recordings per word (synthetic)")
    ap.add_argument("--enroll", type=int, default=3, help="templates per word")
    ap.add_argument("--snr", type=float, default=25.0, help="synthetic SNR for the main test")
    ap.add_argument("--k", type=float, default=3.0, help="rejection threshold = mean + k*std")
    ap.add_argument("--sweep", action="store_true", help="accuracy vs noise (synthetic)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--rounds", type=int, help="(split) how many times you said the whole word list")
    ap.add_argument("--drop", default="", help="(split) comma list of # to drop, e.g. 5,17")
    ap.add_argument("--min-ms", type=int, default=120, help="(split) ignore sounds shorter than this")
    ap.add_argument("--margin-db", type=float, default=CFG["margin_db"], help="(split) loudness above noise floor")
    ap.add_argument("--quiet-frames", type=int, default=CFG["quiet"], help="(split) frames of silence that end a word")
    ap.add_argument("--dry-run", action="store_true", help="(split) show what was found, write nothing")
    ap.add_argument("--force", action="store_true", help="(split) write even if the count looks wrong")
    args = ap.parse_args()

    if args.split:
        cfg = dict(CFG, margin_db=args.margin_db, quiet=args.quiet_frames)
        drop = [d for d in args.drop.split(",") if d.strip()]
        split_session(args.split, args.speaker, args.order, args.out, cfg, args.rounds,
                      args.min_ms, drop, args.dry_run, args.force)
        return
    os.makedirs(args.out, exist_ok=True)
    selftest()
    print(f"band centers (Hz): {', '.join(f'{c:.0f}' for c in CENTERS)}   Q = {BAND_Q:.2f}")

    sweep = []
    t0 = time.time()
    if args.data:
        ds = load_dataset(args.data)
        if not ds:
            sys.exit("no <word>_<k>.wav files found")
        for spk in ds:
            missing = [w for w in WORDS if len(ds[spk][w]) <= args.enroll]
            if missing:
                print(f"{spk}: need more than {args.enroll} recordings of: {', '.join(missing)}")
        ds = {s: d for s, d in ds.items() if all(len(d[w]) > args.enroll for w in WORDS)}
        res = evaluate(ds, ds, args.enroll, k_thr=args.k)
        report(res, f"REAL RECORDINGS ({len(ds)} speaker(s))")
        spk = next(iter(ds))
        example = tuple(ds[spk]["seven"][:2]) if all(ds[spk]["seven"][:2]) else None
    else:
        print("synthetic mode: proves the code path, not real speech accuracy")
        vocab = make_vocab()
        ds = make_synth(vocab, args.reps, args.snr, args.seed)
        oov = make_oov(20, args.snr, args.seed + 100)
        res = evaluate(ds, ds, args.enroll, k_thr=args.k, oov=oov)
        report(res, f"SYNTHETIC, 3 speakers, 19 words, {args.reps - args.enroll} test words each, "
                    f"SNR {args.snr:.0f} dB")
        example = tuple(ds["spk_B"]["five"][:2])
        if args.sweep:
            clean = make_synth(vocab, args.reps, 30, args.seed + 7)
            print("\nSNR sweep (accuracy %):  SNR   enroll-quiet/test-noisy   enroll-in-noise")
            for snr in (30, 20, 15, 10, 5, 0):
                noisy = make_synth(vocab, args.reps, snr, args.seed + 50 + snr)
                r1 = evaluate(clean, noisy, args.enroll, k_thr=args.k)
                r2 = evaluate(noisy, noisy, args.enroll, k_thr=args.k)
                sweep.append((snr, r1["acc"], r2["acc"]))
                print(f"                         {snr:>3} dB        {100 * r1['acc']:5.1f}"
                      f"                    {100 * r2['acc']:5.1f}")
    print(f"\nelapsed {time.time() - t0:.0f} s")
    try:
        if example is not None:
            make_figure(os.path.join(args.out, "dtw_poc_results.png"), example, res["conf"], sweep)
    except ImportError:
        print("matplotlib not installed; skipping figure")


if __name__ == "__main__":
    main()