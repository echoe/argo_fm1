#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Which LEAD value matches Synth1 best? Renders a Synth1 patch once, then LEAD with the importer's output and one LEAD parameter
swept; prints the importer's pick and the best one found. Used to pin down the importer's laws (tools/sy1_ab.py judges the result).

  python3 tools/sy1_calibrate.py          (S1RENDER and SY1_RENDER as for sy1_ab.py)"""
import os, subprocess, sys, tempfile
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sy1_ab as ab

PE0 = 106
ID = dict(CUT=PE0 + 6, RES=PE0 + 7, SAT=68, KEY=67, FAMT=76, FATK=77, FDEC=78, FSUS=79, FREL=80, FM=61, MAMT=71, MATK=72, MDEC=73,
          LDPIT=13, LDFLT=14, LDSHP=15, LDAMP=16, LRATE=9, DETUNE=44, SPRD=95, SUB=62, ATK=1, DEC=2, SUS=3, REL=4, PTCH=PE0 + 2, DETN=PE0 + 3)

def synth1(v, note=60, hold=2.0, tail=1.0):
    with tempfile.TemporaryDirectory() as d:
        sy, wv = os.path.join(d, "p.sy1"), os.path.join(d, "o.wav")
        open(sy, "w").write(ab.sy1_text(v))
        subprocess.run([ab.S1, sy, wv, "-n", str(note), "-v", "100", "-hold", str(hold), "-tail", str(tail), "-voices", "8"], check=True, stdout=subprocess.DEVNULL)
        conv = subprocess.run(["node", os.path.join(ab.HERE, "sy1_convert.mjs"), sy], check=True, capture_output=True, text=True).stdout
        return ab.read_wav(wv), conv

def lead(params_text, over, note=60, hold=2.0, tail=1.0):
    with tempfile.TemporaryDirectory() as d:
        pa, raw = os.path.join(d, "p.txt"), os.path.join(d, "o.f32")
        open(pa, "w").write(params_text + "".join("%d %d\n" % (ID[k] if isinstance(k, str) else k, x) for k, x in over.items()))
        subprocess.run([ab.LD, pa, raw, str(note), "100", str(hold), str(tail)], check=True)
        return np.fromfile(raw, dtype=np.float32).astype(np.float64)

def distance(a, b, times):
    n = min(len(a), len(b)); a, b = a[:n], b[:n]
    ra, rb = ab.level(a), ab.level(b)
    m = min(len(ra), len(rb)); ra, rb = ra[:m], rb[:m]
    gain = ab.db(rb.max()) - ab.db(ra.max())
    la, lb = ab.db(ra), ab.db(rb) - gain
    ok = la > la.max() - 45
    ld = float(np.sqrt(((la - lb)[ok] ** 2).mean())) if ok.any() else 0.0
    bd = []
    for t in times:
        sa, sb = ab.spectrum(a, t), ab.spectrum(b, t)
        if sa is None or sb is None or sa[0].max() < 1e-4 or sb[0].max() < 1e-4: continue
        ba, bb = ab.bands(sa), ab.bands(sb)
        bb = bb * (ba.max() / max(bb.max(), 1e-12))
        sel = ab.db(ba) > ab.db(ba).max() - 50
        bd.append(float(np.sqrt(((ab.db(ba) - ab.db(bb))[sel] ** 2).mean())))
    return ld + (float(np.mean(bd)) if bd else 0.0), gain

def sweep(label, v, key, cands, times=(0.3, 1.0, 1.6), note=60, hold=2.0, tail=1.0):
    a, conv = synth1(v, note, hold, tail)
    pick = None
    for line in conv.splitlines():
        i, x = map(int, line.split())
        if i == (ID[key] if isinstance(key, str) else key): pick = x
    res = []
    for c in cands:
        b = lead(conv, {key: c}, note, hold, tail)
        res.append((distance(a, b, times)[0], c))
    best = min(res)
    d_pick = [d for d, c in res if c == pick]
    print("%-46s importer %4s (d %s)   best %4d (d %.1f)" % (label, pick, "%.1f" % d_pick[0] if d_pick else "-", best[1], best[0]))
    return pick, best[1]

if __name__ == "__main__":
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    P = ab.P
    if which in ("all", "cutoff"):
        for ft in (0, 1, 2, 3, 4):
            for c in (40, 64, 90, 110):
                sweep("cutoff: type %d, Synth1 %d" % (ft, c), ab.patch(FTYPE=ft, CUT=c, RES=0, OSC1=1), "CUT", range(0, 128, 1))
    if which in ("all", "res"):
        for ft in (0, 1, 2, 3, 4):
            for r in (30, 60, 90):
                sweep("resonance: type %d, Synth1 %d" % (ft, r), ab.patch(FTYPE=ft, CUT=64, RES=r, OSC1=1), "RES", range(0, 128, 2))
    if which in ("all", "sat"):
        for s in (30, 60, 100):
            sweep("saturation %d (LP24, cutoff 80)" % s, ab.patch(SAT=s, CUT=80), "SAT", range(0, 128, 2))
    if which in ("all", "key"):
        for k in (32, 64, 100, 127):
            sweep("key track %d (note 72)" % k, ab.patch(FKEY=k, CUT=50, FTYPE=1), "KEY", range(0, 128, 2), note=72)
