#!/usr/bin/env python3
"""
demo.py -interactive proof of concept for the voice-controlled calculator.

It runs the same chain the hardware will: I2S mic model (32 kHz, 18-bit words)
-> unpack -> half-band FIR /2 -> 16 kHz -> DC block -> 12-band filter bank
-> 16 ms feature frames -> endpointing -> DTW against enrolled templates ->
5-bit word code -> calculator / timer / clock logic.

  python demo.py enroll enroll/            build templates from enroll/<word>_<k>.wav
  python demo.py recognize clip.wav        recognize one clip (one word or a sentence)
  python demo.py recognize clip.wav --bypass --show     filter off, open the plot window
  python demo.py loop                      interactive: drop in WAV files one after another
  python demo.py test test/                accuracy on test/<word>_<k>.wav

Keep dtw_poc.py and pcm_sim.py in the same folder. Words in a sentence need
about 0.3 s of silence between them.
"""
import argparse, datetime, glob, os, pickle, re, sys
from math import gcd
import numpy as np
from scipy import signal
from scipy.io import wavfile
import dtw_poc as dtw
import pcm_sim as pcm

WORDS = dtw.WORDS
CODE = {w: i for i, w in enumerate(WORDS)}      # 5-bit word codes, 0..18
REJECT = 31                                     # 11111 = not recognized
DIGITS = {w: str(i) for i, w in enumerate(WORDS[:10])}
TAPS = pcm.halfband_taps()
TEMPLATES = "templates.pkl"
OUT = "demo_out"


def bits(word):
    return format(CODE.get(word, REJECT), "05b")


# --------------------------------------------------------------------- audio
def read_wav(path):
    sr, x = wavfile.read(path)
    x = x.astype(np.float64)
    if x.ndim > 1:
        x = x.mean(axis=1)
    return x, sr


def record_live(seconds=2.5, sr=48000):
    """Record from the laptop mic (needs: pip install sounddevice)."""
    import sounddevice as sd
    print(f"  recording {seconds:.1f} s ... speak now")
    x = sd.rec(int(seconds * sr), samplerate=sr, channels=1, dtype="float64")
    sd.wait()
    return x[:, 0], sr


def run_chain(x, sr, bypass=False, peak_dbfs=-30.0):
    """Everything between the microphone and the feature frames."""
    x = x / (np.max(np.abs(x)) + 1e-9)
    g = gcd(pcm.FS_MIC, int(sr))
    x32 = signal.resample_poly(x, pcm.FS_MIC // g, int(sr) // g) * 10 ** (peak_dbfs / 20)
    words = pcm.to_words(x32)                                    # 18-bit samples << 14
    y16 = pcm.fir_decim2(pcm.unpack(words), TAPS, bypass)        # unpack, FIR, /2
    q, total = dtw.band_features(y16 / 32768.0)                  # DC block, 12 bands, frames
    floor = float(np.percentile(total, 10))
    thr = max(floor + dtw.CFG["margin_db"], dtw.CFG["floor_db"])
    segs = [s for s in dtw.find_segments(total) if s[1] - s[0] >= dtw.CFG["min_frames"]]
    return dict(x32=x32[:len(words)], n_words=len(words), y16=y16, q=q, total=total,
                floor=floor, thr=thr, segs=segs)


def seg_feat(q, s, e):
    f = q[s:e][: dtw.CFG["max_frames"]].astype(np.float32) / 2.55
    return f - f.mean(axis=0, keepdims=True)                     # subtract the word's mean


# ---------------------------------------------------------------- enrollment
def word_from_file(path, peak):
    x, sr = read_wav(path)
    info = run_chain(x, sr, False, peak)
    if not info["segs"]:
        return None
    s, e = max(info["segs"], key=lambda se: se[1] - se[0])
    return seg_feat(info["q"], s, e)


def cmd_enroll(folder, k, peak):
    templates = []
    for fp in sorted(glob.glob(os.path.join(folder, "*.wav"))):
        m = re.match(r"([a-z]+)_(\d+)\.wav$", os.path.basename(fp).lower())
        if not m or m.group(1) not in CODE:
            print("  skipped (name must be <word>_<number>.wav):", os.path.basename(fp))
            continue
        f = word_from_file(fp, peak)
        if f is None:
            print("  no word found in", os.path.basename(fp))
            continue
        templates.append((m.group(1), f))
    if len(templates) < 2:
        sys.exit("not enough usable recordings")
    thr = dtw.loo_threshold(templates, dtw.CFG["band"], k)
    pickle.dump(dict(templates=templates, thr=thr), open(TEMPLATES, "wb"))
    counts = {w: sum(1 for t in templates if t[0] == w) for w in WORDS}
    have = [w for w in WORDS if counts[w]]
    print(f"enrolled {len(templates)} templates, {len(have)}/19 words, rejection threshold {thr:.2f} dB")
    miss = [w for w in WORDS if counts[w] == 0]
    if miss:
        print("  no recordings for:", ", ".join(miss))
    flash = sum(len(f) for _, f in templates) * dtw.NB
    print(f"  template storage: {flash} bytes of flash on the MCU")


def load_db():
    if not os.path.exists(TEMPLATES):
        sys.exit("no templates yet: run  python demo.py enroll <folder>")
    return pickle.load(open(TEMPLATES, "rb"))


# ------------------------------------------------------------- recognition
def classify(feat, db):
    per = {}
    for w, t in db["templates"]:
        d = dtw.dtw_distance(feat, t)
        if d < per.get(w, np.inf):
            per[w] = d
    best = min(per, key=per.get)
    ok = np.isfinite(per[best]) and per[best] <= db["thr"]
    return best, per, bool(ok)


def apply_op(a, op, b):
    r = a + b if op == "plus" else a - b if op == "minus" else a * b
    return None if abs(r) > 99_999_999 else r                    # 8-digit display limit


def interpret(words):
    """The calculator / timer / clock logic that runs after recognition."""
    if words and words[0] == "clock":
        return "CLOCK  " + datetime.datetime.now().strftime("%H:%M:%S")
    if words and words[0] == "timer":
        digs = "".join(DIGITS[w] for w in words[1:] if w in DIGITS)
        started = "start" in words and digs
        return f"TIMER  {int(digs or 0)} s" + ("  (counting down)" if started else "  (say start)")
    acc, op, cur, shown = None, None, "", "0"
    for w in words:
        if w in DIGITS:
            if len(cur) < 6:
                cur += DIGITS[w]
            shown = cur
        elif w in ("plus", "minus", "multiply"):
            if cur:
                acc = int(cur) if op is None or acc is None else apply_op(acc, op, int(cur))
                if acc is None:
                    return "CALC  ERR"
                cur, shown = "", str(acc)
            op = w
        elif w == "equals":
            if cur and op and acc is not None:
                acc = apply_op(acc, op, int(cur))
                if acc is None:
                    return "CALC  ERR"
                cur, op, shown = "", None, str(acc)
        elif w == "clear":
            acc, op, cur, shown = None, None, "", "0"
    return "CALC  " + shown


def recognize_clip(x, sr, db, bypass, peak, plot_path=None, show=False, verbose=True):
    info = run_chain(x, sr, bypass, peak)
    dur = len(x) / sr
    if verbose:
        print(f"\n[1] Input             {dur:.2f} s at {sr} Hz")
        print(f"[2] Mic model         resampled to 32 kHz, quantized to 18-bit words (sample << 14): "
              f"{info['n_words']} words")
        print(f"[3] Unpack + FIR /2   16-bit samples -> 31-tap half-band FIR (Q15) "
              f"{'BYPASSED' if bypass else 'ON'} -> {len(info['y16'])} samples at 16 kHz")
        print(f"[4] Filter bank       DC blocker + 12 band-pass biquads -> {len(info['q'])} frames x 12 bands "
              f"(16 ms each, 8-bit log energy)")
        print(f"[5] Endpointing       noise floor {info['floor']:.0f} dB, threshold {info['thr']:.0f} dB, "
              f"{len(info['segs'])} word(s) found")
    results = []
    for n, (s, e) in enumerate(info["segs"]):
        feat = seg_feat(info["q"], s, e)
        best, per, ok = classify(feat, db)
        ranked = sorted(per.items(), key=lambda kv: kv[1])
        results.append(dict(word=best if ok else None, best=best, d=per[best], per=per, ok=ok,
                            seg=(s, e), feat=feat, ranked=ranked))
        if verbose:
            r2 = f"{ranked[1][0]} {ranked[1][1]:.2f}" if len(ranked) > 1 else "-"
            print(f"[6] DTW word {n + 1}       frames {s}-{e} ({(e - s) * 16} ms): best '{best}' {per[best]:.2f} dB, "
                  f"runner-up {r2}, reject above {db['thr']:.2f}  ->  "
                  f"{best.upper() if ok else 'REJECTED'}  code {bits(best) if ok else format(REJECT, '05b')}")
    heard = [r["word"] for r in results if r["word"]]
    if verbose:
        print("[7] Codes sent to display logic:", " ".join(bits(r["word"]) if r["word"] else format(REJECT, "05b")
                                                            for r in results) or "(none)")
        print("    Heard:", " ".join(heard) or "(nothing)")
        print("    ==>", interpret(heard))
    if plot_path or show:
        make_plot(plot_path, info, run_chain(x, sr, not bypass, peak), results, db, bypass, show)
    return heard, results


def make_plot(path, info, other, results, db, bypass, show):
    import matplotlib
    if not show:
        matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(2, 3, figsize=(16, 9))
    t32 = np.arange(len(info["x32"])) / pcm.FS_MIC
    ax[0, 0].plot(t32, info["x32"], lw=0.5)
    for s, e in info["segs"]:
        ax[0, 0].axvspan(s * 0.016, e * 0.016, color="tab:green", alpha=0.25)
    ax[0, 0].set_title("1. Mic output (32 kHz), detected words shaded")
    ax[0, 0].set_xlabel("time (s)")
    for lab, d, c in (("FIR " + ("OFF" if bypass else "ON"), info, "tab:blue"),
                      ("FIR " + ("ON" if bypass else "OFF"), other, "tab:orange")):
        f, p = signal.welch(d["y16"] / 32768.0, fs=pcm.FS_OUT, nperseg=512)
        ax[0, 1].plot(f, 10 * np.log10(p + 1e-14), label=lab, color=c, lw=1)
    ax[0, 1].legend(); ax[0, 1].set_xlabel("Hz")
    ax[0, 1].set_title("2. After /2 decimation: spectrum, filter on vs off")
    tf = np.arange(len(info["total"])) * 0.016
    ax[0, 2].plot(tf, info["total"], lw=1)
    ax[0, 2].axhline(info["thr"], color="r", ls="--", label="loud threshold")
    ax[0, 2].axhline(info["floor"], color="gray", ls=":", label="noise floor")
    for s, e in info["segs"]:
        ax[0, 2].axvspan(s * 0.016, e * 0.016, color="tab:green", alpha=0.25)
    ax[0, 2].legend(); ax[0, 2].set_xlabel("time (s)"); ax[0, 2].set_ylabel("dB")
    ax[0, 2].set_title("3. Frame energy and endpointing")
    ax[1, 0].imshow(info["q"].T, aspect="auto", origin="lower", cmap="magma",
                    extent=[0, len(info["q"]) * 0.016, -0.5, 11.5])
    ax[1, 0].set_title("4. 12-band features (16 ms frames)")
    ax[1, 0].set_xlabel("time (s)"); ax[1, 0].set_ylabel("band (low to high)")
    if results:
        r = results[0]
        tmpl = min((t for w, t in db["templates"] if w == r["best"]),
                   key=lambda t: dtw.dtw_distance(r["feat"], t))
        path_, C = dtw.dtw_path(r["feat"], tmpl)
        ax[1, 1].imshow(C.T, origin="lower", aspect="auto", cmap="viridis")
        ax[1, 1].plot([p[0] for p in path_], [p[1] for p in path_], "w-", lw=2)
        ax[1, 1].set_title(f"5. DTW alignment: spoken word 1 vs template '{r['best']}'")
        ax[1, 1].set_xlabel("spoken frame"); ax[1, 1].set_ylabel("template frame")
        names = [w for w in WORDS if w in r["per"]]
        vals = [r["per"][w] for w in names]
        cols = ["tab:green" if w == r["best"] else "tab:gray" for w in names]
        ax[1, 2].bar(range(len(names)), vals, color=cols)
        ax[1, 2].axhline(db["thr"], color="r", ls="--", label="reject threshold")
        ax[1, 2].set_xticks(range(len(names))); ax[1, 2].set_xticklabels(names, rotation=90, fontsize=8)
        ax[1, 2].set_title("6. DTW distance to each word (lower = closer)")
        ax[1, 2].set_ylabel("dB per band per step"); ax[1, 2].legend()
    else:
        ax[1, 1].axis("off"); ax[1, 2].axis("off")
    fig.tight_layout()
    if path:
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        fig.savefig(path, dpi=110)
        print("    plot saved:", path)
    if show:
        plt.show()
    plt.close(fig)


# ------------------------------------------------------------------- modes
def cmd_test(folder, db, peak):
    ok = rej = tot = 0
    conf = {}
    for fp in sorted(glob.glob(os.path.join(folder, "*.wav"))):
        m = re.match(r"([a-z]+)_(\d+)\.wav$", os.path.basename(fp).lower())
        if not m or m.group(1) not in CODE:
            continue
        truth = m.group(1)
        f = word_from_file(fp, peak)
        tot += 1
        if f is None:
            rej += 1; print(f"  {os.path.basename(fp):18s} no word found"); continue
        best, per, accepted = classify(f, db)
        if not accepted:
            rej += 1; print(f"  {os.path.basename(fp):18s} REJECTED (best '{best}' {per[best]:.2f})"); continue
        if best == truth:
            ok += 1
        else:
            conf[(truth, best)] = conf.get((truth, best), 0) + 1
            print(f"  {os.path.basename(fp):18s} heard '{best}'  (expected '{truth}')")
    print(f"\n{ok}/{tot} correct = {100 * ok / max(tot, 1):.1f}%   ({rej} rejected, {tot - ok - rej} confused)")
    if conf:
        print("confusions:", ", ".join(f"{a}->{b} x{n}" for (a, b), n in sorted(conf.items(), key=lambda kv: -kv[1])))


def cmd_loop(db, args):
    print("Interactive mode. Type or drag in a WAV path. Blank line = record from the laptop mic. q = quit.")
    n = 0
    while True:
        try:
            s = input("\nWAV file> ").strip().strip('"').strip("'")
        except EOFError:
            break
        if s.lower() in ("q", "quit", "exit"):
            break
        try:
            x, sr = record_live() if s == "" else read_wav(s)
        except Exception as ex:
            print("  could not read input:", ex); continue
        n += 1
        recognize_clip(x, sr, db, args.bypass, args.peak, os.path.join(OUT, f"clip{n}.png"), args.show)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["enroll", "recognize", "loop", "test"])
    ap.add_argument("path", nargs="?")
    ap.add_argument("--bypass", action="store_true", help="skip the anti-alias FIR")
    ap.add_argument("--show", action="store_true", help="open the plot window")
    ap.add_argument("--noplot", action="store_true")
    ap.add_argument("--peak", type=float, default=-30.0, help="mic peak level in dBFS")
    ap.add_argument("--k", type=float, default=3.0, help="reject threshold = mean + k*std")
    a = ap.parse_args()
    if a.mode == "enroll":
        cmd_enroll(a.path, a.k, a.peak)
    elif a.mode == "test":
        cmd_test(a.path, load_db(), a.peak)
    elif a.mode == "loop":
        cmd_loop(load_db(), a)
    else:
        x, sr = read_wav(a.path)
        recognize_clip(x, sr, load_db(), a.bypass, a.peak, None if a.noplot else os.path.join(OUT, "last.png"), a.show)


if __name__ == "__main__":
    main()