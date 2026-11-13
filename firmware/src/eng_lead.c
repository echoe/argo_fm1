/* SPDX-License-Identifier: GPL-3.0-only */
/* LEAD: a two-oscillator subtractive lead / bass / pad voice. It replaces NOISE in engine slot 11 (the number stays: stored
 * projects and presets keep pointing at it).
 *
 * This is a fixed-point re-implementation of the architecture and of the behaviour Synth2 (chab/oxo_synth2, MIT) measured on
 * the original; no Synth2 code is used (it is double-precision C++). What it takes from those measurements:
 *   - OSC 1 shapes SIN SAW PLS TRI, OSC 2 shapes SAW PLS TRI NOIS; pulse width from the track's SHP (64 = square);
 *   - MIX is a linear crossfade OSC 1 -> OSC 2; the SUB (an octave below OSC 1, phase-locked to it, SIN TRI SAW SQR) takes
 *     weight w = SUB / (SUB + 32) off OSC 1 and OSC 2 (OSC 1 (1-w)(1-m), SUB w(1-m), OSC 2 (1-w) m, over 1 - w m);
 *   - SYNC resets OSC 2 to phase 0 whenever OSC 1's phase drops (naive reset, not band-limited, as the original); RING
 *     replaces OSC 2 by OSC 2 x OSC 1 and switches FM off;
 *   - FM: OSC 2 moves OSC 1's phase by a step per sample, (1001^(FM/127) - 1) / 2000 of a cycle at C4, falling to half per
 *     octave up the keyboard (not a ratio of the pitch);
 *   - the mod envelope (an attack and a decay to 0) moves OSC 2's pitch (+-60 semitones at full amount), the FM
 *     depth (room-left: it adds up to the knob's 127) or the pulse width; the amount is -64..+63, 0 = off;
 *   - the filter: LP12, LP24, HP12, BP12 and LPDL (an approximation of the diode ladder: LP24 with a soft knee between its
 *     stages), then a saturation, with a key tracking (KEY 64 = a half: the cutoff follows the key at 50 %);
 *   - OSC 2 key tracking OFF = fixed at 220 Hz plus PTCH and DETN.
 * Left out against the original: the OSC 1 detune copies (the track's UNISON / DETUNE / SPREAD are the host's), the velocity
 * switch of the filter, the saturation curve itself (a blend of the dry signal and a soft clip, not the original's rational curve).
 * The amp / filter envelopes (ADSR, ENV -> FLT), LFOs, glide, UNISON and the effects are the host's.
 *
 * EDIT 1 (OSC):    WAV1  OSC 1 shape      SIN SAW PLS TRI
 *                  WAV2  OSC 2 shape      SAW PLS TRI NOIS
 *                  PTCH  OSC 2 pitch      -24 .. +24 semitones
 *                  DETN  OSC 2 detune     -50 .. +50 cents
 * (EDIT 2, which would hold E4..E7, is hidden for LEAD: its MIX and FILTER pages below take them, ui.c page_visible)
 * MIX:             MIX   0 = OSC 1 (and SUB) only, 127 = OSC 2 only
 *                  XMOD  OFF SYNC RING S+R
 *                  SAT   saturation
 * OSC 2 (FM operator slots, as SWARM's: lead_xdesc names them):
 *                  FM    OSC 2 -> OSC 1 phase modulation, 0 .. 127
 *                  SUB   the sub oscillator's level          SUBW  its shape
 *                  KTRK  OSC 2 key tracking ON / OFF
 * FILTER:          FTYP  LP12 LP24 HP12 BP12 LPDL
 *                  CUT   the filter's cutoff (the track's ENV / LFO -> FLT move it)
 *                  RES   resonance               KEY  key tracking
 * MOD ENV:         MAMT  amount -64 .. +63   MATK  attack   MDEC  decay   MDST  OSC2 (pitch), FM, PW
 *
 * State: s[0] s[1] the first filter stage, s[2] s[3] the second, s[4] the noise generator, s[5] the mod envelope's level
 * (Q16), s[6] its stage (0 attack, 1 decay); ph[0] OSC 1, ph[1] OSC 2, ph[2] the sub's half-cycle (0 / 1). */
static const char *const N_LD_W1[] = {"SIN", "SAW", "PLS", "TRI"};
static const char *const N_LD_W2[] = {"SAW", "PLS", "TRI", "NOIS"};
static const char *const N_LD_XM[] = {"OFF", "SYNC", "RING", "S+R"};
static const char *const N_LD_SUBW[] = {"SIN", "TRI", "SAW", "SQR"};
static const char *const N_LD_FT[] = {"LP12", "LP24", "HP12", "BP12", "LPDL"};
static const char *const N_LD_KT[] = {"OFF", "ON"};
static const char *const N_LD_MD[] = {"OSC2", "FM", "PW"};
enum { LDX_SYNC = 1, LDX_RING = 2 };

#define LDP_FM P_FM1_ATK         /* the slots (lead_xdesc, params.c PAGES "OSC 2", "FILTER", "MOD ENV") */
#define LDP_SUB P_FM1_DEC
#define LDP_SUBW P_FM1_SUS
#define LDP_KTRK P_FM1_REL
#define LDP_FTYPE P_FM2_ATK
#define LDP_KEY P_FM2_DEC
#define LDP_SAT P_FM2_SUS
#define LDP_MAMT P_FM3_ATK
#define LDP_MATK P_FM3_DEC
#define LDP_MDEC P_FM3_SUS
#define LDP_MDST P_FM3_REL
#define LD_NX 11                 /* the slots a preset sets (LEAD_X) */

static const param_desc_t LD_TD[P_FM4_LEVEL - P_FM1_ATK + 1] = {
    [LDP_FM - P_FM1_ATK] = {"FM", F_PCT, 0, 127, 0, 0, 0},
    [LDP_SUB - P_FM1_ATK] = {"SUB", F_PCT, 0, 127, 0, 0, 0},
    [LDP_SUBW - P_FM1_ATK] = {"SUBW", F_ENUM, 0, 3, 1, N_LD_SUBW, 0},
    [LDP_KTRK - P_FM1_ATK] = {"KTRK", F_ENUM, 0, 1, 1, N_LD_KT, 0},
    [LDP_FTYPE - P_FM1_ATK] = {"FTYP", F_ENUM, 0, 4, 1, N_LD_FT, 0},
    [LDP_KEY - P_FM1_ATK] = {"KEY", F_PCT, 0, 127, 64, 0, 0},
    [LDP_SAT - P_FM1_ATK] = {"SAT", F_PCT, 0, 127, 0, 0, 0},
    [LDP_MAMT - P_FM1_ATK] = {"MAMT", F_INT, -64, 63, 0, 0, 0},
    [LDP_MATK - P_FM1_ATK] = {"MATK", F_PCT, 0, 127, 0, 0, 0},
    [LDP_MDEC - P_FM1_ATK] = {"MDEC", F_PCT, 0, 127, 40, 0, 0},
    [LDP_MDST - P_FM1_ATK] = {"MDST", F_ENUM, 0, 2, 0, N_LD_MD, 0},
};
/* LEAD's name for the FM operator slot id (params.c track_desc / param_desc_of), 0 = not one of its own */
static const param_desc_t *lead_xdesc(uint32_t id)
{
    const param_desc_t *d = &LD_TD[id - P_FM1_ATK];
    return d->label ? d : 0;
}

/* the FM step at C4, Q16 cycles / sample, (1001^(v/127) - 1) / 2000 */
static const uint16_t LD_FM16[128] = {
    0, 2, 4, 6, 8, 10, 13, 15, 18, 21, 24, 27, 30, 34, 37, 41,
    45, 50, 54, 59, 64, 70, 76, 82, 88, 95, 102, 110, 118, 126, 135, 144,
    154, 165, 176, 187, 199, 212, 226, 241, 256, 272, 289, 307, 326, 346, 367, 390,
    413, 438, 465, 492, 522, 553, 586, 620, 657, 695, 736, 779, 824, 872, 923, 976,
    1033, 1092, 1155, 1221, 1292, 1366, 1444, 1526, 1613, 1705, 1803, 1905, 2014, 2128, 2249, 2376,
    2511, 2653, 2803, 2962, 3129, 3306, 3493, 3690, 3898, 4118, 4350, 4595, 4854, 5127, 5415, 5720,
    6042, 6381, 6740, 7118, 7518, 7940, 8386, 8857, 9354, 9878, 10432, 11018, 11635, 12288, 12976, 13704,
    14472, 15283, 16139, 17043, 17998, 19006, 20070, 21194, 22381, 23634, 24957, 26354, 27829, 29387, 31031, 32768,
};

/* the deviation at pitch p (1/16 semitone): halves per octave above C4, doubles below (the fraction of an octave
 * linearly: within 6 % of 2^x); at most half a cycle a sample */
static uint32_t lead_fm_dev(uint32_t base, int32_t pitch16)
{
    int32_t x = pitch16 - 60 * 16;
    uint32_t d = base, o, r;
    if (x >= 0) {
        o = (uint32_t)x / 192u;
        r = (uint32_t)x % 192u;
        d >>= o > 31u ? 31u : o;
        d -= (d / 384u) * r;
    } else {
        o = (uint32_t)(-x) / 192u;
        r = (uint32_t)(-x) % 192u;
        d <<= o > 8u ? 8u : o;
        d += (d / 192u) * r * 7u / 10u;
    }
    return d > 32768u ? 32768u : d;
}

/* a mod envelope time (0..127) in samples: 1 ms .. 4 s, quadratic */
static inline int32_t lead_ms(int32_t v) { return 44 + v * v * 11; }

/* the SVF's three outputs, tsvf_lp's steps (dsp.c): 0 LP, 1 BP, 2 HP; k the damping Q12 */
static inline int32_t lead_tap(const tsvf_t *c, int32_t k, int32_t in, int32_t *ic1, int32_t *ic2, uint32_t ft)
{
    int32_t v3 = in - *ic2;
    int32_t v1 = (c->a1 * *ic1 + c->a2 * v3) >> 13;
    int32_t v2 = *ic2 + ((c->a2 * *ic1 + c->a3 * v3) >> 13);
    *ic1 = clamp(2 * v1 - *ic1, -150000, 150000);
    *ic2 = clamp(2 * v2 - *ic2, -150000, 150000);
    if (!ft)
        return v2;
    return ft == 1u ? v1 : in - ((k * v1) >> 12) - v2;
}

static void lead_note_on(track_t *t, voice_t *v)
{
    (void)t;
    v->s[0] = v->s[1] = v->s[2] = v->s[3] = 0;
    v->s[4] = (int32_t)(((v->note + 1u) * 0x9E3779B9u) ^ ((uint32_t)v->fine * 0x85EBCA6Bu) ^ 0x2545F491u);
    if (!v->s[4])
        v->s[4] = 1;
    v->s[5] = 0;                                      /* the mod envelope restarts */
    v->s[6] = 0;
}

static void lead_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t w1 = (uint32_t)p[P_E0] & 3u, w2 = (uint32_t)p[P_E1] & 3u, xm = (uint32_t)p[P_E5] & 3u, i;
    uint32_t subw = (uint32_t)p[LDP_SUBW] & 3u, ktrk = p[LDP_KTRK] != 0, ft = (uint32_t)clamp(p[LDP_FTYPE], 0, 4), mdst = (uint32_t)clamp(p[LDP_MDST], 0, 2);
    int32_t ptch = p[P_E2], det = p[P_E3], mix = p[P_E4], fm = p[LDP_FM], sub = p[LDP_SUB], sat = p[LDP_SAT], mamt = p[LDP_MAMT];
    int32_t cut = (p[P_E6] << 8) + m->cutoff + (((m->pitch16 - 60 * 16) * p[LDP_KEY] * 151) >> 10);
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], tog = v->ph[2] & 1u, inc1 = m->inc, inc2;
    uint32_t pw = 0x80000000u + ((uint32_t)(m->shape - (64 << 8)) << 15);
    uint32_t dev16 = 0, sync = xm & LDX_SYNC, ring = (xm & LDX_RING) != 0, subinc = inc1 >> 1;
    int32_t ic1 = v->s[0], ic2 = v->s[1], ic3 = v->s[2], ic4 = v->s[3], nst = v->s[4];
    int32_t lvl = v->s[5], g = 0, p2 = ((ktrk ? m->pitch16 : 57 * 16) + ptch * 16), fmv = fm;
    int32_t wsub = sub ? (sub << 15) / (sub + 32) : 0, m2 = mix * 258, nrm, cA, cS, cB, k1;
    tsvf_t f1, f2;

    /* the mod envelope (a block at a time): the attack rises linearly, the decay falls by a constant factor */
    if (mamt) {
        int32_t mq = mamt < 0 ? mamt * 512 : mamt * 520;
        if (v->s[6] == 0) {
            lvl += (65536 * (int32_t)n) / lead_ms(p[LDP_MATK]);
            if (lvl >= 65535) {
                lvl = 65535;
                v->s[6] = 1;
            }
        } else {
            uint32_t rn = 65536u - (uint32_t)(452198 / lead_ms(p[LDP_MDEC])), k;
            for (k = n; k > 1u; k >>= 1)
                rn = (rn * rn) >> 16;
            lvl = (int32_t)(((uint32_t)lvl * rn) >> 16);
        }
        g = (mq * lvl) >> 16;                         /* Q15, signed */
        if (mdst == 0)
            p2 += (g * 960) >> 15;                    /* +-60 semitones */
        else if (mdst == 1)
            fmv = clamp(fm + ((g * 127) >> 15), 0, 127);
        else
            pw = (uint32_t)clamp((int32_t)(pw >> 1) + (g << 13), 0x00800000, 0x7F800000) << 1;
    }
    if (ptch || det || (mdst == 0 && mamt) || !ktrk)
        inc2 = cents_inc(p2, det, m->fine);
    else
        inc2 = inc1;
    if (fmv > 0 && !ring)
        dev16 = lead_fm_dev(LD_FM16[fmv], m->pitch16);
    {   /* the mix: OSC 1 (1-w)(1-m), SUB w (1-m), OSC 2 (1-w) m, over 1 - w m (they sum to one) */
        int32_t one = 32768;
        nrm = (1 << 30) / (one - ((m2 * wsub) >> 15));
        cA = ((((one - wsub) * (one - m2)) >> 15) * nrm) >> 15;
        cS = (((wsub * (one - m2)) >> 15) * nrm) >> 15;
        cB = ((((one - wsub) * m2) >> 15) * nrm) >> 15;
    }
    tsvf_coef(&f1, cut, p[P_E7]);
    tsvf_coef(&f2, cut, 0);
    k1 = 8192 - p[P_E7] * 7600 / 127;                 /* tsvf_coef's damping (HP and BP taps) */
    for (i = 0; i < n; i++) {
        int32_t a, b, bb, sb = 0, s, y;
        uint32_t o0 = ph0;
        switch (w1) {
        case 0:
            a = osc_sine(ph0);
            break;
        case 1:
            a = osc_saw(ph0, inc1);
            break;
        case 2:
            a = osc_pulse(ph0, inc1, pw);
            break;
        default:
            a = osc_tri(ph0);
            break;
        }
        switch (w2) {
        case 0:
            b = osc_saw(ph1, inc2);
            break;
        case 1:
            b = osc_pulse(ph1, inc2, pw);
            break;
        case 2:
            b = osc_tri(ph1);
            break;
        default:
            b = (int32_t)(noise32(&nst) >> 16) - 32768;
            break;
        }
        if (wsub) {                                   /* the sub: an octave down, locked to OSC 1's cycle */
            uint32_t sp = (o0 >> 1) | (tog << 31);
            switch (subw) {
            case 0:
                sb = osc_sine(sp);
                break;
            case 1:
                sb = osc_tri(sp);
                break;
            case 2:
                sb = osc_saw(sp, subinc);
                break;
            default:
                sb = osc_pulse(sp, subinc, 0x80000000u);
                break;
            }
        }
        bb = b;
        if (ring)
            b = mulq15(a, b);
        ph0 += inc1;
        if (ph0 < o0)
            tog ^= 1u;
        if (dev16)
            ph0 += (uint32_t)shl((bb * (int32_t)dev16) >> 15, 16);   /* OSC 2 moves OSC 1's phase */
        ph1 += inc2;
        if (sync && ph0 < o0)                         /* OSC 1's phase dropped (a wrap, or FM backwards) */
            ph1 = 0;
        s = mulq15(a, cA) + mulq15(sb, cS) + mulq15(b, cB);
        s >>= 1;
        if (ft == 0u) {
            y = lead_tap(&f1, k1, s, &ic1, &ic2, 0);
        } else if (ft == 1u) {
            y = tsvf_lp(&f2, lead_tap(&f1, k1, s, &ic1, &ic2, 0), &ic3, &ic4);
        } else if (ft == 2u) {
            y = lead_tap(&f1, k1, s, &ic1, &ic2, 2);
        } else if (ft == 3u) {
            y = lead_tap(&f1, k1, s, &ic1, &ic2, 1);
        } else {                                      /* LPDL: LP24 with a soft knee between the stages */
            y = lead_tap(&f1, k1, s, &ic1, &ic2, 0);
            y = tsvf_lp(&f2, soft_knee(y, 9000), &ic3, &ic4);
        }
        y = soft_knee(y, 16000);
        if (sat)                                      /* the saturation: the signal and its soft clip, by SAT */
            y = (y * (127 - sat) + (softclip(y * 6) >> 1) * sat) / 127;
        out[i] += voice_amp(y * 2, m, i) * 2;
    }
    v->ph[0] = ph0;
    v->ph[1] = ph1;
    v->ph[2] = tog;
    v->s[0] = ic1;
    v->s[1] = ic2;
    v->s[2] = ic3;
    v->s[3] = ic4;
    v->s[4] = nst;
    v->s[5] = lvl;
}

/* name, {WAV1, WAV2, PTCH, DETN, MIX, XMOD, CUT, RES}, {A D S R}, fenv, mono. The factory sounds are this tree's own designs */
static const preset_t LEAD_PRESETS[] = {
    {"LD SAW", {1, 1, 0, 9, 64, 0, 80, 25}, {0, 60, 100, 45}, 25, 1, FX(0, 15, 40, 30), PAT(4)},
    {"LD SYNC", {1, 0, 7, 0, 127, 1, 90, 30}, {0, 70, 100, 40}, 30, 1, FX(0, 10, 40, 25), PAT(4)},
    {"LD PULSE", {2, 0, 0, 10, 50, 0, 75, 30}, {0, 55, 100, 40}, 20, 1, FX(0, 20, 40, 30), PAT(4)},
    {"LD FM", {0, 2, 12, 0, 0, 0, 95, 10}, {0, 60, 100, 40}, 15, 1, FX(0, 10, 35, 25), PAT(4)},
    {"LD RING", {1, 0, 5, 3, 64, 2, 85, 20}, {0, 65, 100, 40}, 20, 1, FX(0, 10, 40, 30), PAT(4)},
    {"LD MONO", {1, 1, -12, 6, 60, 0, 70, 35}, {0, 60, 100, 35}, 35, 1, FX(5, 10, 25, 20), PAT(4)},
    {"ACID", {1, 0, 0, 0, 0, 0, 40, 90}, {0, 60, 0, 25}, 60, 1, FX(10, 0, 15, 10), PAT(2)},
    {"LD 303", {1, 0, 0, 0, 0, 0, 35, 80}, {0, 65, 0, 30}, 55, 1, FX(15, 0, 10, 10), PAT(2)},
    {"SUB BASS", {0, 0, 0, 0, 0, 0, 50, 10}, {0, 60, 100, 25}, 20, 1, FX(0, 0, 0, 10), PAT(8)},
    {"FM BASS", {0, 2, 0, 0, 0, 0, 60, 20}, {0, 60, 70, 25}, 25, 1, FX(8, 0, 5, 10), PAT(2)},
    {"PLUCK BS", {1, 1, -12, 5, 64, 0, 45, 30}, {0, 60, 0, 20}, 50, 1, FX(0, 0, 5, 10), PAT(2)},
    {"RING BASS", {2, 0, -12, 0, 50, 2, 55, 30}, {0, 55, 80, 25}, 30, 1, FX(10, 0, 5, 10), PAT(2)},
    {"SQR BASS", {2, 0, -12, 0, 40, 0, 55, 25}, {0, 60, 100, 30}, 35, 1, FX(0, 0, 5, 10), PAT(2)},
    {"PAD SAW", {1, 1, 0, 12, 64, 0, 60, 15}, {70, 80, 110, 95}, 10, 0, FX(0, 50, 30, 60), PAT(5)},
    {"PAD PWM", {2, 2, 0, 7, 60, 0, 65, 15}, {60, 85, 110, 90}, 8, 0, FX(0, 55, 25, 65), PAT(5)},
    {"PAD FM", {0, 2, 7, 0, 0, 0, 85, 10}, {75, 90, 105, 95}, 5, 0, FX(0, 45, 30, 70), PAT(5)},
    {"PAD NOISE", {1, 3, 0, 0, 30, 0, 60, 20}, {80, 90, 100, 100}, 15, 0, FX(0, 40, 35, 75), PAT(5)},
    {"PAD SYNC", {1, 1, 7, 4, 100, 1, 70, 15}, {65, 85, 105, 90}, 12, 0, FX(0, 50, 30, 65), PAT(5)},
    {"PAD HP", {1, 1, 0, 10, 64, 0, 45, 10}, {75, 90, 110, 100}, 0, 0, FX(0, 55, 30, 70), PAT(5)},
    {"CHOIR", {3, 0, 12, 6, 45, 0, 55, 25}, {70, 85, 110, 95}, 8, 0, FX(0, 60, 30, 70), PAT(5)},
    {"EPIANO", {0, 2, 12, 0, 0, 0, 100, 10}, {0, 85, 20, 65}, 15, 0, FX(0, 30, 25, 45), PAT(6)},
    {"CLAV", {2, 0, 0, 0, 40, 0, 70, 30}, {0, 65, 0, 25}, 50, 0, FX(5, 10, 20, 25), PAT(6)},
    {"ORGAN", {0, 0, 12, 0, 50, 0, 127, 0}, {0, 40, 127, 30}, 0, 0, FX(0, 25, 15, 35), PAT(6)},
    {"LD BELL", {0, 2, 19, 0, 60, 2, 100, 20}, {0, 95, 0, 80}, 10, 0, FX(0, 25, 35, 60), PAT(7)},
    {"KALIMBA", {0, 0, 12, 0, 40, 0, 110, 5}, {0, 70, 0, 50}, 10, 0, FX(0, 15, 30, 50), PAT(7)},
    {"LD PLUCK", {1, 1, 0, 6, 64, 0, 50, 30}, {0, 65, 0, 30}, 55, 0, FX(0, 15, 35, 30), PAT(6)},
    {"LD STAB", {1, 1, 0, 12, 70, 0, 65, 25}, {0, 60, 40, 30}, 40, 0, FX(0, 20, 30, 35), PAT(6)},
    {"LD ZAP", {1, 0, 0, 0, 127, 1, 100, 40}, {0, 75, 0, 30}, 10, 0, FX(5, 0, 40, 30), PAT(3)},
    {"NOISE HIT", {1, 3, 0, 0, 127, 0, 70, 40}, {0, 40, 0, 30}, 40, 0, FX(10, 0, 30, 30), PAT(0)},
    {"WIND", {1, 3, 0, 0, 127, 0, 60, 70}, {90, 100, 110, 110}, 0, 0, FX(0, 40, 40, 70), PAT(0)},
    {"LD LASER", {0, 2, 0, 0, 0, 0, 110, 20}, {0, 70, 0, 40}, 0, 0, FX(0, 10, 40, 35), PAT(3)},
    {"LD RISER", {1, 0, 7, 5, 127, 1, 85, 30}, {100, 110, 127, 60}, 20, 0, FX(0, 35, 35, 60), PAT(0)},
};
/* the slots of each preset (lead_track_loaded): FM, SUB, SUBW, KTRK, FTYP, KEY, SAT, MAMT, MATK, MDEC, MDST */
static const int8_t LEAD_X[NELEM(LEAD_PRESETS)][LD_NX] = {
    {0, 0, 1, 1, 1, 64, 0, 0, 0, 40, 0},   /* LD SAW */
    {0, 0, 1, 1, 1, 64, 0, 40, 0, 60, 0},   /* LD SYNC */
    {0, 0, 1, 1, 1, 64, 20, 0, 0, 40, 0},   /* LD PULSE */
    {70, 0, 1, 1, 1, 64, 0, 0, 0, 40, 0},   /* LD FM */
    {0, 0, 1, 1, 1, 64, 0, 0, 0, 40, 0},   /* LD RING */
    {0, 30, 2, 1, 1, 64, 10, 0, 0, 40, 0},   /* LD MONO */
    {0, 0, 1, 1, 1, 64, 40, 0, 0, 40, 0},   /* ACID */
    {0, 0, 1, 1, 4, 64, 30, 0, 0, 40, 0},   /* LD 303 */
    {0, 100, 0, 1, 0, 64, 0, 0, 0, 40, 0},   /* SUB BASS */
    {60, 0, 1, 1, 1, 64, 0, 0, 0, 40, 0},   /* FM BASS */
    {0, 20, 2, 1, 1, 64, 0, 0, 0, 40, 0},   /* PLUCK BS */
    {0, 0, 1, 1, 1, 64, 25, 0, 0, 40, 0},   /* RING BASS */
    {0, 25, 3, 1, 0, 64, 0, 0, 0, 40, 0},   /* SQR BASS */
    {0, 0, 1, 1, 0, 64, 0, 0, 0, 40, 0},   /* PAD SAW */
    {0, 0, 1, 1, 0, 64, 0, 18, 50, 100, 2},   /* PAD PWM */
    {35, 0, 1, 1, 1, 64, 0, 0, 0, 40, 0},   /* PAD FM */
    {0, 0, 1, 1, 3, 64, 0, 0, 0, 40, 0},   /* PAD NOISE */
    {0, 0, 1, 1, 1, 64, 0, 20, 75, 95, 0},   /* PAD SYNC */
    {0, 0, 1, 1, 2, 64, 0, 0, 0, 40, 0},   /* PAD HP */
    {0, 40, 0, 1, 3, 64, 0, 0, 0, 40, 0},   /* CHOIR */
    {25, 0, 1, 1, 1, 64, 0, 40, 0, 55, 1},   /* EPIANO */
    {0, 0, 1, 1, 1, 64, 10, 0, 0, 40, 0},   /* CLAV */
    {0, 70, 0, 1, 0, 64, 0, 0, 0, 40, 0},   /* ORGAN */
    {0, 0, 1, 1, 1, 64, 0, 0, 0, 40, 0},   /* LD BELL */
    {40, 0, 1, 1, 1, 64, 0, 30, 0, 40, 1},   /* KALIMBA */
    {0, 0, 1, 1, 1, 64, 0, 0, 0, 40, 0},   /* LD PLUCK */
    {0, 0, 1, 1, 1, 64, 15, 0, 0, 40, 0},   /* LD STAB */
    {50, 0, 1, 1, 1, 64, 0, -50, 0, 50, 0},   /* LD ZAP */
    {0, 0, 1, 1, 3, 64, 10, 0, 0, 40, 0},   /* NOISE HIT */
    {0, 0, 1, 1, 3, 64, 0, 0, 0, 40, 0},   /* WIND */
    {80, 0, 1, 1, 1, 64, 0, 0, 0, 50, 1},   /* LD LASER */
    {0, 0, 1, 1, 1, 64, 0, 50, 105, 110, 0},   /* LD RISER */
};

static const engine_t ENG_LEAD = {
    .name = "LEAD",
    .page_title = {"OSC", "MIX"},
    .edit = {
        {"WAV1", F_ENUM, 0, 3, 1, N_LD_W1, 0},
        {"WAV2", F_ENUM, 0, 3, 1, N_LD_W2, 0},
        {"PTCH", F_SEMI, -24, 24, 0, 0, 0},
        {"DETN", F_INT, -50, 50, 9, 0, "ct"},
        {"MIX", F_PCT, 0, 127, 64, 0, 0},
        {"XMOD", F_ENUM, 0, 3, 0, N_LD_XM, 0},
        {"CUT", F_CUTOFF, 0, 127, 80, 0, 0},
        {"RES", F_PCT, 0, 127, 25, 0, 0},
    },
    .presets = LEAD_PRESETS,
    .npresets = NELEM(LEAD_PRESETS),
    .note_on = lead_note_on,
    .render = lead_render,
    .knob = {P_E6, P_E7, P_E4, P_REL},
    .keep = 0x0F,                /* the filter */
};

/* the preset a track just loaded (ui.c apply_preset_to, motion.c, tests/hostsim.c): its slots */
static void lead_track_loaded(track_t *t)
{
    static const uint8_t ID[LD_NX] = {LDP_FM, LDP_SUB, LDP_SUBW, LDP_KTRK, LDP_FTYPE, LDP_KEY, LDP_SAT, LDP_MAMT, LDP_MATK, LDP_MDEC, LDP_MDST};
    const int8_t *x;
    uint32_t i;
    if (t->eng_req != ENGI_LEAD)
        return;
    x = LEAD_X[(uint32_t)t->preset % NELEM(LEAD_X)];
    for (i = 0; i < LD_NX; i++)
        t->p[ID[i]] = x[i];
}
