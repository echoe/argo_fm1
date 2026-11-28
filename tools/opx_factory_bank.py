# SPDX-License-Identifier: GPL-3.0-only
"""OPX-4 fork: the factory bank (tools/opx_factory.py compiles it). One P() per patch, in SLOT order: the first NORMAL
(opx_factory.py: 8) in every OPX firmware, the rest in OPX Solo only. The six-operator bank of OPX 0.2 (126 patches) is
in the git history; these eight are its patches reworked for four operators (and one new one), original sounds with
none a copy of a Korg program.

O(level, r=ratio, det=cents, a=, d=, rl= ms, s= %, c= curve, fb=, vel=, ...): an operator (opx_factory.O).
Every patch gives the MOD WHEEL something to do. Levels: a modulator's LEVEL L at full envelope swings the phase by
4 pi (L / 100)^3 rad (50: 1.6 rad, 60: 2.7, 70: 4.3, 80: 6.4). The eight algorithms (carriers in brackets; OP4 is the top
modulator, OP1 the lowest carrier):
  1 [1]     4>3>2>1             2 [1]     3,4>2>1          3 [1]     4,(3>2)>1        4 [1]     (4>3),2>1
  5 [1 3]   2>1 4>3             6 [1 2 3] 4>1,2,3         7 [1 2 3] 4>3              8 [1..4]
"""
from opx_factory import C, EG, FLT, FX, LFO, O, OFF, P, VP

R1 = 7          # (coarse index of ratio 1: the factory patches of 0.2.0 by their codes, unchanged)


def OPC(l, co, fi, dt, a, d, r, s, cv, fb, v):        # (an operator as eng_opx.c's OP macro had it)
    return O(l, (co, fi), det=dt, a=C(a), d=C(d), rl=C(r), s=s, c=cv, fb=fb, vel=v)


OFFC = OPC(0, R1, 1000, 0, 0, 0, 0, 0, 0, 0, 0)

# ====================================================================== F1..F7: the 0.2.0 patches on four operators ==
P("E.PIANO", "K", 5, [
    OPC(100, R1, 1000, 2, 1, 682, 300, 0, 8, 0, 60), OPC(62, R1, 1000, -2, 1, 620, 300, 0, 8, 0, 70),
    OPC(80, R1, 1000, -3, 1, 764, 400, 0, 8, 0, 60), OPC(58, R1 + 13, 1000, 3, 1, 450, 300, 0, 8, 0, 80)],
  vp=[VP("MW", "OP4.LEVEL", 25)], fx=[FX("PAN", 35, 1, 3.5, 50), FX("3EQ", 2, -1, 0, 1000)],
  sends=(0, 40, 20, 35), pat="KEYS", note="two pairs: a body (op 1, 2) and a tine (op 3, 4); MW the tine")
P("BELL", "U", 5, [
    OPC(100, R1, 1000, 0, 1, 806, 806, 0, 10, 0, 50), OPC(52, R1 + 2, 1410, 0, 1, 764, 764, 0, 10, 0, 60),
    OPC(85, R1, 1000, 4, 1, 764, 764, 0, 10, 0, 50), OPC(48, R1 + 5, 1000, 0, 1, 682, 682, 0, 10, 0, 60)],
  fx=[FX("3EQ", -3, 2, 0, 1000), FX("PAN", 20, 1, 0.3, 50)],
  sends=(0, 10, 30, 70), pat="BELL", note="inharmonic modulators")
P("FM BASS", "B", 5, [
    OPC(100, R1 - 1, 1000, 0, 1, 682, 207, 70, 6, 0, 40), OPC(78, R1 - 1, 1000, 0, 1, 476, 207, 30, 6, 0, 60),
    OPC(80, R1 - 1, 1000, 1, 1, 620, 207, 60, 6, 0, 40), OPC(70, R1, 1000, 0, 1, 414, 207, 20, 6, 0, 60)],
  eg=[None, EG(d=C(512), rl=C(414), s=0, c=4)],
  flt=FLT("LPF24", C(600), reso=20, eg2=36, eg2_c="VEL", kh=43, kc=48),
  fx=[FX("COMP", 100, 40, 30, -6)], sends=(0, 0, 10, 10), mono=1, pat="OFFBEAT", note="two pairs")
P("BRASS", "L", 6, [
    OPC(100, R1, 1000, 0, 351, 620, 300, 85, 4, 0, 30), OPC(90, R1, 1000, 6, 351, 620, 300, 85, 4, 0, 30),
    OPC(90, R1, 1000, -6, 351, 620, 300, 85, 4, 0, 30), OPC(74, R1, 1000, 0, 414, 600, 300, 65, 4, 45, 50)],
  eg=[None, EG(a=C(394), d=C(574), rl=C(512), s=50, c=4)], lfo=[LFO("SIN", C(701), ks=2, fade=600)], plfo=0.12,
  flt=FLT("LPF12", C(651), reso=10, eg2=30, eg2_c="VEL", kh=64, kc=60),
  fx=[FX("3EQ", 0, -2, 3, 1500)], sends=(0, 25, 20, 40), pat="LEAD", note="one modulator on three carriers")
P("SAW LEAD", "L", 8, [
    OPC(100, R1, 1000, 0, 1, 682, 207, 90, 3, 75, 20), OPC(90, R1, 1000, 7, 1, 682, 207, 90, 3, 75, 20),
    OPC(90, R1, 1000, -7, 1, 682, 207, 90, 3, 75, 20), OFFC],
  eg=[None, EG(d=C(538), rl=C(476), s=30, c=6)], lfo=[LFO("SIN", C(701), ks=1)], plfo=0.5, plfo_c="MW",
  flt=FLT("LPF24", C(765), reso=30, eg2=18, kh=64, kc=60),
  fx=[FX("DIST", 25, -8, 2500, 2)], sends=(10, 30, 30, 30), mono=1, pat="LEAD",
  note="three detuned carriers at the sawtooth-like feedback")
P("FM PAD", "P", 1, [
    OPC(100, R1, 1000, 0, 600, 826, 682, 90, 5, 0, 20), OPC(45, R1, 1000, 3, 620, 826, 682, 80, 5, 0, 30),
    OPC(40, R1 + 1, 1000, 0, 656, 826, 682, 70, 5, 0, 30), OPC(38, R1, 1000, -3, 682, 826, 682, 70, 5, 55, 30)],
  eg=[None, EG(a=C(656), d=C(718), rl=C(656), s=60, c=5)], lfo=[None, LFO("TRI", C(333), ks=1)],
  flt=FLT("LPF12", C(715), reso=15, eg2=12, kh=0, kc=60, lfo2=8),
  fx=[FX("PHA", 50, 70, 0.15, 40), FX("PAN", 30, 1, 0.1, 50)], sends=(0, 60, 30, 70), pat="PAD",
  note="the serial chain 4 > 3 > 2 > 1, OP4 on its own feedback")
P("X-MOD", "F", "USER", [
    OPC(100, R1, 1000, 0, 1, 764, 414, 50, 6, 0, 40), OPC(90, R1 + 1, 1005, 0, 1, 764, 414, 50, 6, 0, 40),
    OPC(50, R1 + 2, 1000, 0, 1, 558, 414, 10, 6, 0, 60), OFFC],
  ua=[[0, 12, 0, 0], [10, 0, 0, 0], [35, 35, 0, 0], [0] * 4], out="1 2",
  lfo=[None, None, LFO("TRI", C(378), ks=2)],
  vp=[VP("MW", "OP3.LEVEL", 30), VP("LFO3", "OP2.PITCH", 0.1)],
  fx=[FX("FLTR", 65, 40, 35, 0.5), FX("DEC", 25, 16000, 12.0, 0)], sends=(0, 20, 30, 40), pat="MELODY",
  note="USER: two carriers that modulate each other, a third modulating both")

# ============================================================================== F8: DRAWBARS in both firmwares (NORMAL 8) ==
P("DRAWBARS", "K", 8, [
    O(80, 0.5, a=3, rl=40), O(85, 1, a=3, rl=40), O(70, 2, a=3, rl=40),
    O(62, 3, a=1, d=450, s=0, rl=40, c=9, vel=40)],
  lfo=[LFO("SIN", 6.8, ks=0)], plfo=0.12, plfo_c="MW",
  fx=[FX("3EQ", 2, 1, -1, 800), FX("PAN", 25, "SINE", 0.8, 50)], sends=(0, 50, 0, 30), pat="KEYS",
  gain=-0.6, note="16' 8' 4' and a decaying 2 2/3' (percussion); MW a scanner-like vibrato")
