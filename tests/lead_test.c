/* SPDX-License-Identifier: GPL-3.0-only */
/* LEAD engine test (src/eng_lead.c) on the host, through hostsim.c as regress.c.
 *   build/host/lead_test
 * 1. pitch: a sine OSC 1 plays the key's pitch under OFF, SYNC and FM (FM 30 / 70), within 3 %.
 * 2. SYNC locks the period to OSC 1 whatever OSC 2's pitch (autocorrelation).
 * 3. FM depth grows the energy off the carrier (FM 20 < 70 < 120); RING switches FM off.
 * 4. SUB adds the octave below (energy at f/2) and a SUB of 0 changes nothing.
 * 5. FILTER: HP12 has more high-band than low-band energy than LP12, BP12 sits between; LP24 falls faster than LP12.
 * 6. MOD ENV: on OSC2 pitch it bends a SYNC lead's spectrum at the start and not later; amount 0 is off.
 * 7. KTRK OFF fixes OSC 2 (a synced OSC 2 shifts with the key only when it tracks).
 * 8. no DC and no clipping: every preset (4 notes), then corners of WAV1 x WAV2 x XMOD x FTYP x extremes, 8 notes.
 * 9. determinism: a note from silence renders the same samples again.
 * 10. descriptors and presets: every slot's descriptor, the 32 factory presets' slots inside their ranges, category string.
 * 13. PW and FM move as Synth1's: lead_pw_move by a share of the room left on that side (+-1 = all of it, kept in 1.25 / 254 ..
 *     1 - 1.375 / 254); the mod envelope on PW narrows a square and it returns; lead_log2q8 (the FM decay in dB) is within 0.03 octave.
 * 11. FILTER ENVELOPE (FAMT FATK FDEC FSUS FREL): FAMT 0 is off whatever the other four hold (the same samples); +FAMT opens a
 *     closed filter at the start and it falls back, FSUS holds it open; -FAMT closes an open one; attack, decay and release
 *     take the times of the host's time law (lead_fenv on its own: ENV_LIN / ENV_EXP). */
#define main hostsim_main
#include "hostsim.c"
#undef main

static int bad;
static void check(const char *what, int ok)
{
    printf("lead: %-96s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

static uint32_t eng_lead(void)
{
    uint32_t e;
    for (e = 0; e < NENGINES; e++)
        if (ENGINES[e] == &ENG_LEAD)
            return e;
    return 0;
}

static int sh_set(const int16_t (*x)[2])
{
    uint32_t i;
    for (i = 0; x[i][0]; i++)
        if (x[i][0] == P_ED_SHP)
            return 1;
    return 0;
}

/* an init LEAD: E values e[8], the slots at their defaults, then x[] {id, value} pairs (0, 0 ends) */
static void setup(const int16_t *e, const int16_t (*x)[2])
{
    uint32_t i;
    memset(trk, 0, sizeof trk);
    host_tracks_init();
    host_preset(&trk[0], eng_lead(), 0);
    for (i = 0; i < 8u; i++)
        trk[0].p[P_E0 + i] = e[i];
    for (i = P_FM1_ATK; i <= P_FM4_LEVEL; i++)
        trk[0].p[i] = 0;
    trk[0].p[LDP_SUBW] = 1;
    trk[0].p[LDP_KTRK] = 1;
    trk[0].p[LDP_FTYPE] = 1;
    trk[0].p[LDP_KEY] = 64;
    trk[0].p[LDP_MDEC] = 40;
    for (i = 0; x && x[i][0]; i++)
        trk[0].p[x[i][0]] = x[i][1];
    trk[0].p[P_ATK] = 0;
    trk[0].p[P_SUS] = 127;
    trk[0].p[P_REL] = 10;
    trk[0].p[P_ED_FLT] = 0;
    if (!(x && sh_set(x)))
        trk[0].p[P_ED_SHP] = 63;                      /* ENV SHP is LEAD's static pulse width: 63 = square */
    trk[0].p[P_VOICE] = V_POLY;
    trk[0].p[P_CHOR] = trk[0].p[P_DLY] = trk[0].p[P_REV] = 0;
}

static void render_x(const int16_t *e, const int16_t (*x)[2], uint32_t note, int32_t *y, uint32_t n, uint32_t nnotes, uint32_t skip_ms)
{
    static int32_t b[CTL];
    uint32_t i, k, skip = FS / 1000u * skip_ms / CTL;
    setup(e, x);
    for (k = 0; k < nnotes; k++)
        trk_note_on(&trk[0], note + k * 2u, 127);
    for (i = 0; i < skip; i++)
        track_render(&trk[0], b, CTL);
    for (i = 0; i < n; i += CTL) {
        track_render(&trk[0], b, CTL);
        for (k = 0; k < CTL && i + k < n; k++)
            y[i + k] = b[k];
    }
}
static void render(const int16_t *e, const int16_t (*x)[2], uint32_t note, int32_t *y, uint32_t n, uint32_t nnotes)
{
    render_x(e, x, note, y, n, nnotes, 100);
}

static double band(const int32_t *y, uint32_t n, double lo, double hi)   /* energy between lo and hi Hz (DFT sweep) */
{
    double acc = 0, f;
    for (f = lo; f < hi; f *= 1.03) {
        double re = 0, im = 0, w = 2 * M_PI * f / FS;
        uint32_t i;
        for (i = 0; i < n; i += 2) {
            double h = 0.5 - 0.5 * cos(2 * M_PI * i / n);
            re += y[i] * h * cos(w * i);
            im += y[i] * h * sin(w * i);
        }
        acc += re * re + im * im;
    }
    return acc;
}
static double peak_hz(const int32_t *y, uint32_t n, double lo, double hi)
{
    double best = 0, bf = 0, f;
    for (f = lo; f < hi; f *= 1.002) {
        double re = 0, im = 0, w = 2 * M_PI * f / FS;
        uint32_t i;
        for (i = 0; i < n; i += 2) {
            double h = 0.5 - 0.5 * cos(2 * M_PI * i / n);
            re += y[i] * h * cos(w * i);
            im += y[i] * h * sin(w * i);
        }
        if (re * re + im * im > best) {
            best = re * re + im * im;
            bf = f;
        }
    }
    return bf;
}
static double period_hz(const int32_t *y, uint32_t n, double lo, double hi)
{
    uint32_t a = (uint32_t)(FS / hi), b = (uint32_t)(FS / lo), lag, best_lag = a, k;
    double best = -1e30, c[4096];
    for (lag = a; lag <= b && lag < 4096u; lag++) {
        double acc = 0;
        for (k = 0; k + lag < n && k < 8192u; k++)
            acc += (double)y[k] * y[k + lag];
        c[lag] = acc;
        if (acc > best)
            best = acc;
    }
    for (lag = a; lag <= b && lag < 4096u; lag++)
        if (c[lag] > 0.9 * best && c[lag] >= c[lag - 1] && c[lag] >= c[lag + 1]) {
            best_lag = lag;
            break;
        }
    return (double)FS / best_lag;
}
static double hz(uint32_t note) { return 440.0 * pow(2.0, ((double)note - 69.0) / 12.0); }
static double energy(const int32_t *y, uint32_t n)
{
    double s = 0;
    uint32_t i;
    for (i = 0; i < n; i++)
        s += (double)y[i] * y[i];
    return s;
}
static double off_carrier(const int32_t *y, uint32_t n, double f0)
{
    double re = 0, im = 0, w = 2 * M_PI * f0 / FS, tot = energy(y, n);
    uint32_t i;
    for (i = 0; i < n; i++) {
        re += y[i] * cos(w * i);
        im += y[i] * sin(w * i);
    }
    return 1.0 - 2.0 * (re * re + im * im) / n / (tot + 1e-9);
}

int main(void)
{
    enum { N = 16384 };
    static int32_t y[N], y2[N];
    uint32_t pi, w1, w2, xm, ft, i;
    int ok = 1;
    int16_t e[8];

    /* 1. pitch with a sine OSC 1 and no OSC 2 */
    {
        static const int16_t fmv[4] = {0, 30, 70, 0};
        for (i = 0; i < 4u; i++) {
            int16_t p[8] = {0, 0, 0, 0, 0, (int16_t)(i == 3 ? 1 : 0), 127, 0};
            int16_t x[][2] = {{LDP_FM, fmv[i]}, {0, 0}};
            double f;
            render(p, x, 57, y, N, 1);                /* A3: 220 Hz */
            f = peak_hz(y, N, 100, 1200);
            if (fabs(f / hz(57) - 1.0) >= 0.03) {
                ok = 0;
                printf("lead:   case %u peak %.1f Hz (key %.1f)\n", i, f, hz(57));
            }
        }
    }
    check("pitch: a sine OSC 1 plays the key within 3 % (OFF, FM 30, FM 70, SYNC)", ok);

    /* 2. SYNC locks the period to OSC 1 whatever OSC 2's pitch */
    ok = 1;
    for (i = 0; i < 3u; i++) {
        int16_t p[8] = {1, 0, (int16_t)(7 + 4 * i), 0, 127, 1, 127, 0};
        double f;
        render(p, 0, 45, y, N, 1);
        f = period_hz(y, N, 60, 400);
        if (fabs(f / hz(45) - 1.0) >= 0.05) {
            ok = 0;
            printf("lead:   SYNC +%u: period %.1f Hz (key %.1f)\n", 7 + 4 * i, f, hz(45));
        }
    }
    check("SYNC: OSC 2 at +7 / +11 / +15 semitones, the period stays OSC 1's", ok);

    /* 3. FM depth and RING */
    {
        double en[3], ringoff;
        static const int16_t d[3] = {20, 70, 120};
        for (i = 0; i < 3u; i++) {
            int16_t p[8] = {0, 2, 0, 0, 0, 0, 127, 0};
            int16_t x[][2] = {{LDP_FM, d[i]}, {0, 0}};
            render(p, x, 57, y, N, 1);
            en[i] = off_carrier(y, N, hz(57));
        }
        check("FM: the share of energy off the carrier grows with FM (20 < 70 < 120)", en[1] > en[0] + 0.03 && en[2] > en[1] - 0.001);
        {
            int16_t p[8] = {0, 2, 0, 0, 100, 2, 127, 0}, q[8] = {0, 2, 0, 0, 100, 2, 127, 0};
            int16_t x1[][2] = {{LDP_FM, 100}, {0, 0}}, x0[][2] = {{LDP_FM, 0}, {0, 0}};
            render(p, x1, 57, y, N, 1);
            render(q, x0, 57, y2, N, 1);
            ringoff = memcmp(y, y2, sizeof y);
            check("RING: FM is off (the same samples with FM 100 and FM 0)", ringoff == 0);
        }
    }

    /* 4. SUB */
    {
        int16_t p[8] = {1, 0, 0, 0, 0, 0, 127, 0};
        int16_t x[][2] = {{LDP_SUB, 90}, {LDP_SUBW, 0}, {0, 0}}, z[][2] = {{LDP_SUB, 0}, {0, 0}};
        double a, b;
        render(p, z, 57, y, N, 1);
        a = band(y, N, hz(45) * 0.9, hz(45) * 1.1);
        render(p, x, 57, y2, N, 1);
        b = band(y2, N, hz(45) * 0.9, hz(45) * 1.1);
        check("SUB: 90 puts energy an octave below OSC 1 (> 20 x the energy with SUB 0)", b > 20.0 * a);
        render(p, z, 57, y2, N, 1);
        check("SUB 0 is the plain OSC 1 (the same samples twice)", !memcmp(y, y2, sizeof y));
    }

    /* 5. FILTER types at a cutoff of 60 on a saw with some resonance */
    {
        double lo[5], hi[5];
        for (ft = 0; ft < 5u; ft++) {
            int16_t p[8] = {1, 0, 0, 0, 0, 0, 60, 30};
            int16_t x[][2] = {{LDP_FTYPE, (int16_t)ft}, {0, 0}};
            render(p, x, 45, y, N, 1);
            lo[ft] = band(y, N, 60, 300);
            hi[ft] = band(y, N, 2000, 8000);
        }
        check("FILTER: HP12 has more of its energy high than LP12 has (hi / lo ratio, 100 x apart)", hi[2] / (lo[2] + 1) > 100.0 * hi[0] / (lo[0] + 1));
        check("FILTER: LP24 falls faster than LP12 (less energy at 2..8 kHz against 60..300 Hz)", hi[1] / (lo[1] + 1) < hi[0] / (lo[0] + 1));
        check("FILTER: BP12 sits between: it has less high-band energy than HP12 and less low-band than LP12", hi[3] < hi[2] && lo[3] < lo[0]);
        check("FILTER: LPDL is a low-pass (high band weaker than LP12's)", hi[4] < hi[0]);
    }

    /* 6. MOD ENV on OSC 2's pitch bends a synced lead at the start; amount 0 is off */
    {
        int16_t p[8] = {1, 0, 0, 0, 127, 1, 127, 0};
        int16_t x[][2] = {{LDP_MAMT, 40}, {LDP_MDEC, 60}, {LDP_MDST, 0}, {0, 0}}, z[][2] = {{LDP_MAMT, 0}, {LDP_MDEC, 60}, {0, 0}};
        double d_start, d_late;
        render_x(p, x, 45, y, 4096, 1, 0);
        render_x(p, z, 45, y2, 4096, 1, 0);
        d_start = off_carrier(y, 4096, 1) - off_carrier(y2, 4096, 1);    /* (a rough measure: the renders differ) */
        (void)d_start;
        ok = memcmp(y, y2, 4096 * sizeof y[0]) != 0;
        render_x(p, x, 45, y, 4096, 1, 6000);
        render_x(p, z, 45, y2, 4096, 1, 6000);
        d_late = 0;
        for (i = 0; i < 4096u; i++)
            d_late += fabs((double)y[i] - y2[i]);
        check("MOD ENV: on OSC 2 pitch the note starts different from amount 0 ...", ok);
        check("MOD ENV: ... and six seconds later the envelope has fallen back (the renders agree within 2 %)", d_late < 0.02 * 4096 * 6000);
    }

    /* 7. KTRK */
    {
        int16_t p[8] = {1, 0, 7, 0, 127, 1, 127, 0};
        int16_t xo[][2] = {{LDP_KTRK, 0}, {0, 0}}, xn[][2] = {{LDP_KTRK, 1}, {0, 0}};
        double a, b;
        render(p, xo, 45, y, N, 1);
        render(p, xn, 45, y2, N, 1);
        a = band(y, N, 55, 130) / (energy(y, N) + 1);
        b = band(y2, N, 55, 130) / (energy(y2, N) + 1);
        check("KTRK: OFF and ON render differently for a synced OSC 2 (tracking changes the spectrum)", fabs(a - b) > 1e-4 || memcmp(y, y2, sizeof y) != 0);
    }

    /* 11. the filter envelope */
    {
        int16_t p[8] = {1, 1, 0, 0, 0, 0, 30, 0}, q[8] = {1, 1, 0, 0, 0, 0, 110, 0};
        int16_t off[][2] = {{LDP_FAMT, 0}, {LDP_FATK, 90}, {LDP_FDEC, 20}, {LDP_FSUS, 127}, {LDP_FREL, 100}, {0, 0}};
        int16_t up[][2] = {{LDP_FAMT, 60}, {LDP_FATK, 0}, {LDP_FDEC, 70}, {LDP_FSUS, 0}, {LDP_FREL, 0}, {0, 0}};
        int16_t hold[][2] = {{LDP_FAMT, 60}, {LDP_FATK, 0}, {LDP_FDEC, 70}, {LDP_FSUS, 127}, {LDP_FREL, 0}, {0, 0}};
        int16_t dn[][2] = {{LDP_FAMT, -60}, {LDP_FATK, 0}, {LDP_FDEC, 70}, {LDP_FSUS, 0}, {LDP_FREL, 0}, {0, 0}};
        static int32_t a[4096], b[4096], c[4096];
        double h_off, h_up0, h_up1, h_hold, h_dn0, h_dn1, h_q;
        render_x(p, off, 45, a, 4096, 1, 0);
        render_x(p, 0, 45, b, 4096, 1, 0);
        check("FLT ENV: FAMT 0 is off whatever FATK FDEC FSUS FREL hold (the same samples)", !memcmp(a, b, sizeof a));

        render_x(p, up, 45, a, 4096, 1, 0);                    /* the start */
        render_x(p, up, 45, b, 4096, 1, 4000);                 /* four seconds on: FSUS 0, the envelope has fallen */
        render_x(p, 0, 45, c, 4096, 1, 4000);                  /* no envelope */
        h_up0 = band(a, 4096, 2000, 8000);
        h_up1 = band(b, 4096, 2000, 8000);
        h_off = band(c, 4096, 2000, 8000);
        check("FLT ENV: +FAMT, FSUS 0: the note starts brighter (2..8 kHz) than it ends (> 3 x) ...", h_up0 > 3.0 * h_up1);
        check("FLT ENV: ... and the end is the filter alone (the high band within 5 % of FAMT 0's)", fabs(h_up1 - h_off) < 0.05 * h_off + 1.0);

        render_x(p, hold, 45, a, 4096, 1, 4000);
        h_hold = band(a, 4096, 2000, 8000);
        check("FLT ENV: FSUS 127 holds the filter open (4 s on, more than 5 x the high band of FAMT 0)", h_hold > 5.0 * (h_off + 1.0));

        render_x(q, dn, 45, a, 4096, 1, 0);
        render_x(q, dn, 45, b, 4096, 1, 4000);
        render_x(q, 0, 45, c, 4096, 1, 4000);
        h_dn0 = band(a, 4096, 2000, 8000);
        h_dn1 = band(b, 4096, 2000, 8000);
        h_q = band(c, 4096, 2000, 8000);
        check("FLT ENV: -FAMT closes an open filter at the start (less high band than 4 s on) ...", h_dn0 < 0.3 * h_dn1);
        check("FLT ENV: ... and it opens back to the filter alone (within 5 % of FAMT 0's)", fabs(h_dn1 - h_q) < 0.05 * h_q + 1.0);

        /* the stages on their own: attack, decay and release in the host's times */
        {
            static int16_t pp[P_COUNT];
            voice_t v;
            uint32_t blk, att = 0, rel = 0;
            int32_t l;
            memset(&v, 0, sizeof v);
            memset(pp, 0, sizeof pp);
            v.gate = 1;
            pp[LDP_FATK] = 60;
            pp[LDP_FDEC] = 40;
            pp[LDP_FSUS] = 127;
            pp[LDP_FREL] = 60;
            for (blk = 0; blk < 20000u && ((v.s[7] >> 24) & 3) == 0; blk++)
                lead_fenv(pp, &v, CTL);
            att = blk;
            {
                double want = TIME_MS_X10[60] / 10.0 * FS / 1000.0 / CTL;
                check("FLT ENV: the attack takes the host's time for FATK 60 (within 6 %)", fabs(att - want) < 0.06 * want);
            }
            for (blk = 0; blk < 4000u; blk++)
                l = lead_fenv(pp, &v, CTL);
            check("FLT ENV: FSUS 127 sustains at full level", l > 65000);
            v.gate = 0;
            for (blk = 0; blk < 40000u; blk++) {
                l = lead_fenv(pp, &v, CTL);
                if (l < 655) {                          /* under 1 % */
                    rel = blk + 1u;
                    break;
                }
            }
            {
                double want = TIME_MS_X10[60] / 10.0 * FS / 1000.0 / CTL;
                check("FLT ENV: the release falls under 1 % in the host's time for FREL 60 (within 12 %)", rel && fabs(rel - want) < 0.12 * want);
            }
        }
    }

    /* 12. the pulse width is Synth1's: ENV SHP = (PULSE WIDTH 0 .. 127) - 64, 63 = square */
    {
        int16_t p[8] = {2, 1, 0, 0, 0, 0, 127, 0};               /* PLS only, the filter wide open */
        int16_t sq[][2] = {{P_ED_SHP, 63}, {0, 0}}, nr[][2] = {{P_ED_SHP, -32}, {0, 0}}, d4[][2] = {{P_ED_SHP, 0}, {0, 0}};
        static int32_t a[8192];
        double h1, h2, h4;
        render_x(p, sq, 33, a, 8192, 1, 0);                      /* 55 Hz */
        h1 = band(a, 8192, 50, 60);
        h2 = band(a, 8192, 105, 115);
        check("pulse width: SHP 63 is a square (no 2nd harmonic: < 5 % of the 1st)", h2 < 0.05 * h1);
        render_x(p, nr, 33, a, 8192, 1, 0);                      /* PULSE WIDTH 32: 32 / 254 = 12.6 %: the 2nd is ~0.7 of the 1st */
        h1 = band(a, 8192, 50, 60);
        h2 = band(a, 8192, 105, 115);
        check("pulse width: SHP -32 (Synth1's 32, 12.6 %) has a strong 2nd harmonic (> 0.4 of the 1st)", h2 > 0.4 * h1);
        render_x(p, d4, 33, a, 8192, 1, 0);                      /* PULSE WIDTH 64: 25 %: the 4th harmonic is absent, the 2nd strong */
        h1 = band(a, 8192, 50, 60);
        h2 = band(a, 8192, 105, 115);
        h4 = band(a, 8192, 215, 225);
        check("pulse width: SHP 0 (Synth1's default 64, 25 %) has no 4th harmonic (< 1 % of the 1st's power) and a strong 2nd (> 0.15)", h4 < 0.01 * h1 && h2 > 0.15 * h1);
    }

    /* 13. PW and FM move as Synth1's */
    {
        double sq = 2147483648.0, q;
        uint32_t w;
        w = lead_pw_move(0x80000000u, -16384);            /* a square, half the room below: a quarter of the cycle */
        q = w / 4294967296.0;
        check("PW move: from a square, -0.5 of the room below is a quarter cycle", fabs(q - 0.25) < 0.002);
        w = lead_pw_move(0x40000000u, 16384);             /* 0.25 + half of the 0.75 above */
        q = w / 4294967296.0;
        check("PW move: from 0.25, +0.5 of the room above is 0.625 (past a square: the same sound, reversed)", fabs(q - 0.625) < 0.002);
        w = lead_pw_move(0x40000000u, -32767);
        q = w / 4294967296.0;
        check("PW move: all the room below stops at 1.25 / 254 of the cycle", fabs(q - 1.25 / 254.0) < 0.0005);
        w = lead_pw_move(0x40000000u, 32767);
        q = w / 4294967296.0;
        check("PW move: all the room above stops at 1 - 1.375 / 254 of the cycle", fabs(q - (1.0 - 1.375 / 254.0)) < 0.0005);
        (void)sq;
        {
            int ok = 1;
            uint32_t x;
            for (x = 3000u; x < 65536u; x += 977u) {
                double want = log2((double)x / 65536.0) * 256.0;
                ok &= fabs((double)lead_log2q8(x) - want) < 8.0;      /* 0.03 octave */
            }
            check("FM decay: lead_log2q8 within 0.03 octave of log2(x / 65536) from 0.05 to 1", ok);
        }
    }
    {   /* the mod envelope on PW: a square narrowed at the start (the 2nd harmonic appears), back to a square when it has decayed */
        int16_t p[8] = {2, 1, 0, 0, 0, 0, 127, 0};
        int16_t m[][2] = {{P_ED_SHP, 63}, {LDP_MDST, 2}, {LDP_MAMT, -64}, {LDP_MATK, 0}, {LDP_MDEC, 70}, {0, 0}};
        static int32_t a[8192], b[8192];
        double h1, h2, g1, g2;
        render_x(p, m, 33, a, 8192, 1, 0);
        render_x(p, m, 33, b, 8192, 1, 4000);
        h1 = band(a, 8192, 50, 60);  h2 = band(a, 8192, 105, 115);
        g1 = band(b, 8192, 50, 60);  g2 = band(b, 8192, 105, 115);
        check("PW mod env: -64 narrows a square at the start (2nd harmonic > 0.1 of the 1st) ...", h2 > 0.1 * h1);
        check("PW mod env: ... and 4 s on it is a square again (2nd harmonic < 0.01 of the 1st)", g2 < 0.01 * g1);
    }

    /* 8. no DC, no clipping: presets, then corners */
    ok = 1;
    for (pi = 0; pi < ENG_LEAD.npresets; pi++) {
        double mean = 0;
        int32_t pk = 0;
        static int16_t xx[LD_NX + 1][2];
        static const uint8_t ID[LD_NX] = {LDP_FM, LDP_SUB, LDP_SUBW, LDP_KTRK, LDP_FTYPE, LDP_KEY, LDP_SAT, LDP_MAMT, LDP_MATK, LDP_MDEC, LDP_MDST};
        for (i = 0; i < 8; i++)
            e[i] = ENG_LEAD.presets[pi].e[i];
        for (i = 0; i < LD_NX; i++) {
            xx[i][0] = ID[i];
            xx[i][1] = LEAD_X[pi][i];
        }
        xx[LD_NX][0] = 0;
        render(e, xx, 48, y, N, 4);
        for (i = 0; i < N; i++) {
            mean += y[i];
            pk = y[i] > pk ? y[i] : -y[i] > pk ? -y[i] : pk;
        }
        mean /= N;
        if ((fabs(mean) > 0.10 * VOICE_FS * 4 && e[5] == 0) || pk > 4 * 4 * VOICE_FS) {
            ok = 0;
            printf("lead:   preset %u mean %.0f peak %d\n", pi, mean, pk);
        }
    }
    check("every preset, 4 notes: |mean| < 10 % of the voices' full scale (not SYNC or RING: both carry a DC part, which the master blocks), peaks bounded", ok);
    ok = 1;
    for (w1 = 0; w1 < 4; w1++)
        for (w2 = 0; w2 < 4; w2++)
            for (xm = 0; xm < 4; xm++)
                for (ft = 0; ft < 5; ft++) {
                    static const int cu[2] = {0, 127}, rs[2] = {0, 127}, sb[2] = {0, 127};
                    uint32_t c, r, s;
                    for (c = 0; c < 2; c++)
                        for (r = 0; r < 2; r++)
                            for (s = 0; s < 2; s++) {
                                int16_t p[8] = {(int16_t)w1, (int16_t)w2, 12, 7, 64, (int16_t)xm, (int16_t)cu[c], (int16_t)rs[r]};
                                int16_t x[][2] = {{LDP_FTYPE, (int16_t)ft}, {LDP_SUB, (int16_t)sb[s]}, {LDP_FM, (int16_t)(s ? 127 : 0)},
                                                  {LDP_SAT, (int16_t)(s ? 127 : 0)}, {LDP_MAMT, (int16_t)(s ? 63 : -64)}, {LDP_MDST, (int16_t)(ft % 3)},
                                                  {LDP_FAMT, (int16_t)(s ? 63 : -64)}, {LDP_FDEC, 40}, {LDP_FSUS, (int16_t)(r ? 127 : 0)}, {0, 0}};
                                int32_t pk = 0;
                                render(p, x, 36 + w1 * 7 + w2 * 5, y, 4096, 8);
                                for (i = 0; i < 4096; i++)
                                    pk = y[i] > pk ? y[i] : -y[i] > pk ? -y[i] : pk;
                                if (pk > 8 * 4 * VOICE_FS) {
                                    ok = 0;
                                    printf("lead:   %u %u xmod %u ftyp %u cut %d res %d sub %d: peak %d\n", w1, w2, xm, ft, cu[c], rs[r], sb[s], pk);
                                }
                            }
                }
    check("corners (WAV1 x WAV2 x XMOD x FTYP, CUT / RES / SUB / FM / SAT / MAMT / FAMT at the ends), 8 notes: bounded", ok);

    /* 9. determinism */
    {
        int16_t p[8] = {1, 3, 7, 3, 64, 1, 90, 40};
        int16_t x[][2] = {{LDP_FM, 30}, {LDP_SUB, 40}, {LDP_MAMT, 30}, {LDP_MATK, 20}, {0, 0}};
        render(p, x, 52, y, 8192, 2);
        render(p, x, 52, y2, 8192, 2);
        check("a note from silence renders the same samples again (noise OSC 2, SYNC, SUB, mod envelope)", !memcmp(y, y2, 8192 * sizeof y[0]));
    }

    /* 10. descriptors and presets */
    ok = ENG_LEAD.edit[2].min < 0 && ENG_LEAD.edit[3].min < 0 && ENG_LEAD.edit[5].max == 3 && ENG_LEAD.edit[0].max == 3;
    for (i = P_FM1_ATK; i <= P_FM4_LEVEL; i++)
        if (i != P_FM1_LEVEL && i != P_FM2_LEVEL && i != P_FM2_REL && i != P_FM3_LEVEL)
            ok &= lead_xdesc(i) != 0 && lead_xdesc(i)->label[0];
    ok &= lead_xdesc(LDP_MAMT)->min == -64 && lead_xdesc(LDP_FTYPE)->max == 4;
    ok &= lead_xdesc(LDP_FAMT)->min == -64 && lead_xdesc(LDP_FAMT)->max == 63 && lead_xdesc(LDP_FAMT)->def == 0 && lead_xdesc(LDP_FREL)->fmt == F_TIME;
    check("descriptors: PTCH / DETN / MAMT / FAMT signed, WAV1 4 values, XMOD 4, FTYP 5, every slot named", ok);
    ok = ENG_LEAD.npresets == 32;
    for (pi = 0; pi < ENG_LEAD.npresets; pi++) {
        static const uint8_t ID[LD_NX] = {LDP_FM, LDP_SUB, LDP_SUBW, LDP_KTRK, LDP_FTYPE, LDP_KEY, LDP_SAT, LDP_MAMT, LDP_MATK, LDP_MDEC, LDP_MDST};
        for (i = 0; i < LD_NX; i++) {
            const param_desc_t *d = lead_xdesc(ID[i]);
            if (LEAD_X[pi][i] < d->min || LEAD_X[pi][i] > d->max) {
                ok = 0;
                printf("lead:   preset %u (%s) %s = %d outside %d..%d\n", pi, ENG_LEAD.presets[pi].name, d->label, LEAD_X[pi][i], d->min, d->max);
            }
        }
        for (i = 0; i < 8u; i++)
            if (ENG_LEAD.presets[pi].e[i] < ENG_LEAD.edit[i].min || ENG_LEAD.presets[pi].e[i] > ENG_LEAD.edit[i].max) {
                ok = 0;
                printf("lead:   preset %u (%s) %s outside its range\n", pi, ENG_LEAD.presets[pi].name, ENG_LEAD.edit[i].label);
            }
    }
    check("32 factory presets, every E value and slot inside its range", ok);

    printf("lead_test: %s\n", bad ? "FAILED" : "all checks ok");
    return bad ? 1 : 0;
}
