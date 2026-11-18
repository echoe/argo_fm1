#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""A/B the .sy1 importer by ear's measurements: each test patch is rendered by Synth1's engine (fm1-sy1's s1render) and, converted by
the editor's importer (tools/sy1_convert.mjs), by LEAD (tests/sy1_render.c); the two are compared on their level over time (25 ms
frames) and their spectrum (1/3-octave bands, centroid) after the overall gain difference is taken out.

  S1RENDER=.../fm1-sy1/build/s1render SY1_RENDER=build/host/sy1_render python3 tools/sy1_ab.py [-v] [-k NAME]

Exit 1 if a case is off by more than the tolerances at the top (the importer's mapping is checked, not LEAD's loudness: the gain
offset is reported but not judged)."""
import os, re, struct, subprocess, sys, tempfile, wave
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
S1 = os.environ.get("S1RENDER", "s1render")
LD = os.environ.get("SY1_RENDER", os.path.join(HERE, "../build/host/sy1_render"))
FS = 44100
TOL_LEVEL_DB = 3.0       # the level over time, gain removed: the worst frame (above -45 dB of the peak)
TOL_CENTROID_OCT = 0.35  # the spectral centroid, at the checked times
TOL_BANDS_DB = 6.0       # the 1/3-octave bands (above -50 dB of the loudest): their rms difference
# Known, understood differences, by a part of the case's name: (level dB, centroid oct, bands dB). Each is something the importer can't
# map away, not a mapping error:
TOLERANCES = [
    ("saw+saw detuned", (4.5, 0.35, 6.0)),     # two free-running oscillators beat: the phases differ, so the level frame by frame does
    ("+7 semitones", (4.5, 0.35, 6.0)),
    ("filter type 1 cutoff 40", (4.5, 0.35, 6.0)),   # LEAD's LP24 (two SVFs) is ~6 dB louder than Synth1's ladder at low cutoffs (the gain column)
    ("filter type 4 cutoff 40", (4.5, 0.35, 6.0)),
    ("filter env attack", (20.0, 0.35, 22.0)),   # (the more specific names first)
    ("filter env", (20.0, 0.35, 8.0)),         # the same loudness difference moves along the sweep: judged by brightness (centroid, bands)
    ("osc1 FM", (3.0, 0.35, 8.0)),             # the FM itself: LEAD's bands sit ~5 dB off Synth1's at any amount
    ("mod env -> FM", (7.0, 0.45, 9.0)),
    ("mod env -> osc2 pitch", (4.0, 0.35, 6.5)),   # a fast pitch sweep: a few ms of timing is a few dB
    ("LFO on amp", (10.0, 0.35, 6.0)),         # the dips are steep: a few ms of phase is several dB
    ("LFO triangle on filter", (6.5, 0.35, 6.0)),
    ("unison", (8.0, 1.0, 9.0)),               # 8 voices over the same width; Synth1's N voices (here 8: random phases: the sum is noisy)
]

def tolerance(name):
    for part, tol in TOLERANCES:
        if part in name: return tol
    return (TOL_LEVEL_DB, TOL_CENTROID_OCT, TOL_BANDS_DB)

# Synth1 parameter ids (fm1-sy1 s1_tables.h)
P = dict(OSC1=0, OSC2=1, PITCH2=2, FINE2=3, KBD2=4, MIX=5, SYNC=6, RING=7, PW=8, KEYSHIFT=9, MOD_ON=10, MOD_AMT=11, MOD_ATK=12, MOD_DEC=13,
         FTYPE=14, FA=15, FD=16, FS=17, FR=18, CUT=19, RES=20, FAMT=21, FKEY=22, SAT=23, AA=25, AD=26, AS=27, AR=28, PLAY=38, PORTA=39,
         L1DEST=41, L1TYPE=42, L1SPEED=43, L1DEPTH=44, FM=45, L1ON=57, L2ON=58, ARP=59, DELAY=65, CHORUS=66, L1TS=67, L1KS=68, MODDEST=71,
         UNI=73, UNIDET=75, UNIV=93, UNIPAN=84, OSC1DET=76, FX=77, SUBGAIN=95, SUBSHAPE=96, SUBOCT=97)

def init_values():
    txt = open(os.path.join(HERE, "../web/editor.html")).read()
    m = re.search(r"pdef: \[([^\]]*)\]", txt)
    return [int(x) for x in m.group(1).split(",")]

BASE = init_values()

def patch(**kw):
    v = BASE[:]
    # a plain, dry, steady base: one saw, the filter open and unmodulated, an organ-like amp envelope, no effects / arp / unison / LFO / mod env
    v[P["OSC1"]] = 1; v[P["MIX"]] = 0; v[P["OSC2"]] = 1; v[P["MOD_ON"]] = 0; v[P["FTYPE"]] = 1; v[P["CUT"]] = 100; v[P["RES"]] = 0
    v[P["FAMT"]] = 64; v[P["FKEY"]] = 0; v[P["SAT"]] = 0; v[P["AA"]] = 10; v[P["AD"]] = 60; v[P["AS"]] = 127; v[P["AR"]] = 40
    v[P["FA"]] = 0; v[P["FD"]] = 60; v[P["FS"]] = 127; v[P["FR"]] = 40
    for k in ("L1ON", "L2ON", "ARP", "DELAY", "CHORUS", "FX", "UNI", "SUBGAIN", "OSC1DET", "KEYSHIFT", "PORTA", "PLAY", "SYNC", "RING"):
        v[P[k]] = 0
    v[72] = 64  # fine tune: its centre
    for k, x in kw.items():
        v[P[k]] = x
    return v

def sy1_text(v, name="ab"):
    return name + "\nver=112\n" + "\n".join("%d,%d" % (i, x) for i, x in enumerate(v)) + "\n"

def read_wav(path):
    w = wave.open(path)
    n, ch = w.getnframes(), w.getnchannels()
    a = np.frombuffer(w.readframes(n), dtype=np.int16).astype(np.float64) / 32768.0
    return a.reshape(-1, ch).mean(axis=1)

def render_both(v, note=60, hold=2.0, tail=1.0):
    with tempfile.TemporaryDirectory() as d:
        sy, wv, pa, raw = (os.path.join(d, x) for x in ("p.sy1", "o.wav", "p.txt", "o.f32"))
        open(sy, "w").write(sy1_text(v))
        subprocess.run([S1, sy, wv, "-n", str(note), "-v", "100", "-hold", str(hold), "-tail", str(tail), "-voices", "8"], check=True, stdout=subprocess.DEVNULL)
        out = subprocess.run(["node", os.path.join(HERE, "sy1_convert.mjs"), sy], check=True, capture_output=True, text=True).stdout
        open(pa, "w").write(out)
        subprocess.run([LD, pa, raw, str(note), "100", str(hold), str(tail)], check=True)
        a = read_wav(wv)
        b = np.fromfile(raw, dtype=np.float32).astype(np.float64)
    n = min(len(a), len(b))
    return a[:n], b[:n]

def db(x): return 20 * np.log10(np.maximum(x, 1e-9))

def level(x, fr=int(FS * 0.025)):
    n = len(x) // fr
    return np.sqrt((x[:n * fr].reshape(n, fr) ** 2).mean(axis=1))

def spectrum(x, t, n=8192):
    s = int(t * FS)
    seg = x[s:s + n]
    if len(seg) < n: return None
    return np.abs(np.fft.rfft(seg * np.hanning(n))), np.fft.rfftfreq(n, 1 / FS)

def centroid_oct(sp):
    m, f = sp
    k = (f > 40) & (f < 12000)
    return np.log2((m[k] ** 2 * f[k]).sum() / np.maximum((m[k] ** 2).sum(), 1e-18) / 1000.0)

def bands(sp):
    m, f = sp
    edges = 40 * 2 ** (np.arange(0, 28) / 3.0)
    out = []
    for lo, hi in zip(edges[:-1], edges[1:]):
        k = (f >= lo) & (f < hi)
        out.append(np.sqrt((m[k] ** 2).sum()) if k.any() else 0)
    return np.array(out)

def compare(a, b, times):
    ra, rb = level(a), level(b)
    n = min(len(ra), len(rb)); ra, rb = ra[:n], rb[:n]
    gain = db(rb.max()) - db(ra.max())
    la, lb = db(ra), db(rb) - gain
    ok = la > la.max() - 45
    lvl = float(np.abs(la - lb)[ok].max()) if ok.any() else 0.0
    cen, bnd = [], []
    for t in times:
        sa, sb = spectrum(a, t), spectrum(b, t)
        if sa is None or sb is None: continue
        if sa[0].max() < 1e-4 or sb[0].max() < 1e-4: continue
        cen.append(centroid_oct(sb) - centroid_oct(sa))
        ba, bb = bands(sa), bands(sb)
        bb = bb * (ba.max() / max(bb.max(), 1e-12))
        sel = db(ba) > db(ba).max() - 50
        bnd.append(float(np.sqrt(((db(ba) - db(bb))[sel] ** 2).mean())))
    return gain, lvl, (max(map(abs, cen)) if cen else 0.0), (max(bnd) if bnd else 0.0)

CASES = []
def case(name, times=(0.3, 1.0, 1.6), **kw):
    CASES.append((name, patch(**kw), times))

# the oscillators and the filter, steady
for s1 in (0, 1, 2, 3): case("osc1 shape %d" % s1, OSC1=s1, CUT=127, FTYPE=0)
for s2 in (1, 2, 3, 4): case("osc2 shape %d only" % s2, OSC2=s2, MIX=127, CUT=127, FTYPE=0)
case("osc mix 64 saw+saw detuned 7 cents", OSC2=1, MIX=64, FINE2=64 + 7, CUT=127, FTYPE=0)
case("osc2 +7 semitones, mix 64", OSC2=1, MIX=64, PITCH2=52 + 7, CUT=127, FTYPE=0)
case("sync", OSC2=1, MIX=127, SYNC=1, PITCH2=52 + 7, CUT=127, FTYPE=0)
case("ring", OSC2=2, MIX=64, RING=1, PITCH2=52 + 5, CUT=127, FTYPE=0)
for pw in (20, 64, 100, 127): case("pulse width %d" % pw, OSC1=2, PW=pw, CUT=127, FTYPE=0)
case("sub square", SUBGAIN=80, SUBSHAPE=3, SUBOCT=1, CUT=127, FTYPE=0)
case("osc1 FM 40", OSC1=1, FM=40, OSC2=0, MIX=0, CUT=127, FTYPE=0)
for ft in range(5):
    for c in (40, 64, 90):
        case("filter type %d cutoff %d res 20" % (ft, c), FTYPE=ft, CUT=c, RES=20, OSC1=1)
case("resonance 90, LP24, cutoff 60", FTYPE=1, CUT=60, RES=90)
case("saturation 40 (80 is past LEAD's SAT)", SAT=40, CUT=80)
case("filter key track 100%, note 72", FKEY=127, CUT=50, FTYPE=1)
# the envelopes
case("amp ADSR 60/70/60/70", AA=60, AD=70, AS=60, AR=70, times=(0.05, 0.2, 0.6, 1.5, 2.3))
case("amp attack slow 90", AA=90, AD=50, AS=127, AR=50, times=(0.2, 0.6, 1.2, 1.8))
case("filter env +100: decay 75, sustain 20", CUT=30, FAMT=100, FD=75, FS=20, FTYPE=1, times=(0.02, 0.2, 0.5, 1.2))
case("filter env -90, decay 80, sustain 0", CUT=110, FAMT=64 - 90, FD=80, FS=0, FTYPE=0, times=(0.02, 0.2, 0.5, 1.2))
case("filter env attack 80", CUT=30, FAMT=110, FA=80, FD=60, FS=60, FTYPE=1, times=(0.1, 0.4, 0.8, 1.5))
case("filter env release (note off at 2 s)", CUT=30, FAMT=110, FD=60, FS=127, FR=90, AR=100, FTYPE=1, times=(1.0, 2.1, 2.4, 2.8))
# mod envelope
case("mod env -> osc2 pitch", MOD_ON=1, MOD_AMT=64 + 24, MOD_ATK=0, MOD_DEC=80, MODDEST=0, OSC2=1, MIX=64, CUT=127, FTYPE=0, times=(0.03, 0.15, 0.5, 1.2))
case("mod env -> PW", MOD_ON=1, MOD_AMT=64 + 50, MOD_ATK=0, MOD_DEC=80, MODDEST=2, OSC1=2, PW=100, CUT=127, FTYPE=0, times=(0.03, 0.15, 0.5, 1.2))
case("mod env -> FM", MOD_ON=1, MOD_AMT=64 + 40, MOD_ATK=0, MOD_DEC=80, MODDEST=1, FM=30, CUT=127, FTYPE=0, times=(0.03, 0.15, 0.5, 1.2))
# LFO
case("LFO sine on pitch", L1ON=1, L1DEST=2, L1TYPE=5, L1SPEED=60, L1DEPTH=40, CUT=127, FTYPE=0, times=(0.3, 0.7, 1.3))
case("LFO triangle on filter", L1ON=1, L1DEST=3, L1TYPE=1, L1SPEED=60, L1DEPTH=80, CUT=50, FTYPE=1, times=(0.3, 0.7, 1.3))
case("LFO on amp", L1ON=1, L1DEST=4, L1TYPE=5, L1SPEED=60, L1DEPTH=80, CUT=127, FTYPE=0, times=(0.3, 0.7, 1.3))
# unison
case("unison on", UNI=1, UNIDET=40, CUT=127, FTYPE=0)

def main():
    verbose = "-v" in sys.argv
    only = sys.argv[sys.argv.index("-k") + 1] if "-k" in sys.argv else None
    bad = 0
    print("%-44s %8s %8s %8s %8s" % ("case", "gain dB", "level dB", "cent oct", "bands dB"))
    for name, v, times in CASES:
        if only and only not in name: continue
        a, b = render_both(v)
        gain, lvl, cen, bnd = compare(a, b, times)
        tl, tc, tb = tolerance(name)
        flag = "" if lvl <= tl and cen <= tc and bnd <= tb else "  <-- OFF"
        bad += bool(flag)
        print("%-44s %8.1f %8.1f %8.2f %8.1f%s" % (name, gain, lvl, cen, bnd, flag))
    print("%d cases off" % bad if bad else "all cases within tolerance")
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()
