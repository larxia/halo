#!/usr/bin/env python3
"""HALO audio checks on the simulator's WAV files: level, peak, DC, clicks, stereo, spectral balance, NaN/silence.
   tests/analyse.py FILE.wav ...   (exit 1 if a file fails a hard check)"""
import sys, wave, numpy as np
bad = 0
for fn in sys.argv[1:]:
    w = wave.open(fn); n = w.getnframes()
    x = np.frombuffer(w.readframes(n), dtype=np.int16).reshape(-1, 2).astype(np.float64) / 32768
    m = x.mean(1)
    rms = np.sqrt((x ** 2).mean()); peak = np.abs(x).max(); dc = np.abs(x.mean(0)).max()
    d2 = np.abs(np.diff(m, 2)); click = d2.max() / (np.median(d2) + 1e-9)
    corr = np.corrcoef(x[:, 0], x[:, 1])[0, 1] if x.std() > 0 else 1
    F = np.abs(np.fft.rfft(m)) ** 2; f = np.fft.rfftfreq(len(m), 1 / 44100); tot = F.sum() + 1e-20
    bands = [10 * np.log10(F[(f >= lo) & (f < hi)].sum() / tot + 1e-12) for lo, hi in
             [(20, 200), (200, 1000), (1000, 4000), (4000, 10000), (10000, 20000)]]
    cent = (f * F).sum() / tot
    fails = []
    if rms < 0.003: fails.append("silent")
    if peak > 0.999: fails.append("clipped")
    if dc > 0.01: fails.append("dc")
    if not np.isfinite(x).all(): fails.append("nan")
    bad |= bool(fails)
    print(f"{fn.split('/')[-1]:28s} {n/44100:5.1f}s rms {20*np.log10(rms+1e-9):6.1f} dB peak {20*np.log10(peak+1e-9):6.1f} dB "
          f"dc {dc:.4f} LR {corr:+.2f} centroid {cent:6.0f} Hz bands " + " ".join(f"{b:6.1f}" for b in bands) +
          f" click {click:7.0f}" + ("  FAIL " + ",".join(fails) if fails else ""))
sys.exit(1 if bad else 0)
