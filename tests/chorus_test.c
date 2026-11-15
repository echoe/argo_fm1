/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Host test of the CHORUS bus (Argo: a Juno-60 / 106 chorus; src/fx.c fx_buses), on the real sources:
 *   build/host/chorus_test      (run_tests.sh builds and runs it)
 * 1. the three switches (G_CRATE, three parts of 0..127): I 0.513 Hz, II 0.863 Hz, I+II 9.75 Hz, read as the delay a 100 Hz sine
 *    comes out with: its rate and its swing about a 3.3 ms centre (+-1.3, +-1.9, +-0.35 ms at DEPTH 64).
 * 2. stereo: the two channels' delays swing the opposite ways (left = mono + side, right = mono - side); the mono sum is the wet bus.
 * 3. DEPTH scales the swing (0: none), the level stays near the input's, noise stays bounded with no DC.
 * 4. a switch change glides (no click past the sine's own slope); the labels and values the CHORUS page shows. */
#define UI_TEST_NO_MAIN 1
#include "ui_test.c"

static int bad;
static void ck(const char *what, int ok)
{
    printf("chorus: %-120s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        bad++;
}

#define NB (FS / CTL * CTL * 4u)                      /* four seconds, whole blocks */
static int32_t in_s[NB], l_s[NB], r_s[NB];
static void run(uint32_t mode_val, uint32_t depth, const int32_t *in)
{
    static int32_t zero[CTL], wet[CTL];
    uint32_t i, k;
    memset(&fx, 0, sizeof fx);
    memset(dly_buf, 0, sizeof dly_buf);
    memset(cho_buf, 0, sizeof cho_buf);
    memset(rev_comb, 0, sizeof rev_comb);
    memset(rev_ap, 0, sizeof rev_ap);
    song.g[G_CRATE] = (int16_t)mode_val;
    song.g[G_CDEPTH] = (int16_t)depth;
    for (i = 0; i < NB; i += CTL) {
        memset(zero, 0, sizeof zero);
        memset(cho_sd, 0, sizeof cho_sd);
        fx_buses(in + i, zero, zero, wet, CTL);
        for (k = 0; k < CTL; k++) {
            l_s[i + k] = wet[k] + cho_sd[k];
            r_s[i + k] = wet[k] - cho_sd[k];
        }
    }
}
/* the delay (ms) of x against the input, one 100 Hz cycle (441 samples) at a time, from the end of the first second */
#define NW (NB / 441u)
static double dly_l[NW], dly_r[NW];
static void delays(const int32_t *x, double *d)
{
    uint32_t w, i;
    for (w = 0; w < NW; w++) {
        double ci = 0, si = 0, co = 0, so = 0, ph;
        for (i = 0; i < 441u; i++) {
            double c = cos(6.283185307179586 * 100.0 * (double)(w * 441u + i) / FS), s = sin(6.283185307179586 * 100.0 * (double)(w * 441u + i) / FS);
            ci += in_s[w * 441u + i] * c; si += in_s[w * 441u + i] * s;
            co += x[w * 441u + i] * c; so += x[w * 441u + i] * s;
        }
        ph = atan2(so, co) - atan2(si, ci);               /* a delay: the output lags by w t */
        while (ph < -1.0) ph += 6.283185307179586;
        while (ph > 5.28) ph -= 6.283185307179586;
        d[w] = ph / (6.283185307179586 * 100.0) * 1000.0;
    }
}
static void stats(const double *d, double *mean, double *pp, double *hz)
{
    uint32_t w, cross = 0, first = 0, last = 0;
    double mn = 1e9, mx = -1e9, m = 0;
    for (w = 100; w < NW; w++) {
        mn = d[w] < mn ? d[w] : mn;
        mx = d[w] > mx ? d[w] : mx;
        m += d[w];
    }
    m /= (double)(NW - 100u);
    for (w = 101; w < NW; w++)
        if ((d[w - 1] - m < 0) != (d[w] - m < 0)) {
            if (!cross) first = w;
            last = w;
            cross++;
        }
    *mean = m; *pp = mx - mn;
    *hz = cross > 2u ? (double)(cross - 1u) / 2.0 / ((double)(last - first) / 100.0) : 0;     /* a cycle is two crossings */
}

int main(void)
{
    uint32_t i;
    double mean[3], pp[3], hz[3], mr, ppr, hr, cor = 0, sl = 0, sr = 0;
    static const double HZ[3] = {0.513, 0.863, 9.75}, SW[3] = {2.6, 3.8, 0.7};
    static const uint32_t MV[3] = {0, 64, 127};
    int ok;
    for (i = 0; i < NB; i++)
        in_s[i] = (int32_t)(20000.0 * sin(6.283185307179586 * 100.0 * (double)i / FS));
    for (uint32_t m = 0; m < 3u; m++) {
        run(MV[m], 64, in_s);
        delays(l_s, dly_l);
        stats(dly_l, &mean[m], &pp[m], &hz[m]);
        printf("chorus:   mode %u: delay %.2f ms +- %.2f (p-p), %.3f Hz\n", m, mean[m], pp[m] / 2, hz[m]);
        ok = fabs(mean[m] - 3.3) < 0.5 && fabs(pp[m] - SW[m]) < 0.2 * SW[m] + 0.1 && fabs(hz[m] - HZ[m]) < 0.15 * HZ[m] + 0.02;
        {
            char w[96];
            snprintf(w, sizeof w, "switch %s: centre 3.3 ms, swing %.1f ms p-p, %.3f Hz", m == 0 ? "I" : m == 1 ? "II" : "I+II", SW[m], HZ[m]);
            ck(w, ok);
        }
    }
    /* stereo: mode II, the two delays swing opposite ways */
    run(64, 64, in_s);
    delays(l_s, dly_l);
    delays(r_s, dly_r);
    stats(dly_l, &mean[1], &pp[1], &hz[1]);
    stats(dly_r, &mr, &ppr, &hr);
    for (i = 100; i < NW; i++) {
        cor += (dly_l[i] - mean[1]) * (dly_r[i] - mr);
        sl += (dly_l[i] - mean[1]) * (dly_l[i] - mean[1]);
        sr += (dly_r[i] - mr) * (dly_r[i] - mr);
    }
    cor /= sqrt(sl * sr);
    ck("stereo: the left and right delays swing opposite ways (correlation < -0.95), the same swing", cor < -0.95 && fabs(pp[1] - ppr) < 0.3);
    {   /* the level: RMS of the wet against the input's; the mono sum (l + r) / 2 is the bus */
        double a = 0, b = 0;
        for (i = FS; i < NB; i++) {
            a += (double)in_s[i] * in_s[i];
            b += (double)l_s[i] * l_s[i];
        }
        ck("level: the wet is as loud as the input (-1 .. +0.5 dB)", sqrt(b / a) > 0.89 && sqrt(b / a) < 1.06);
    }
    /* DEPTH */
    run(64, 0, in_s);
    delays(l_s, dly_l);
    stats(dly_l, &mean[0], &pp[0], &hz[0]);
    ok = pp[0] < 0.1;
    run(64, 127, in_s);
    delays(l_s, dly_l);
    stats(dly_l, &mean[2], &pp[2], &hz[2]);
    ck("DEPTH 0: no swing; DEPTH 127: about twice the Juno's (or the 140-sample limit)", ok && pp[2] > 1.6 * 3.8 * 0.9 && pp[2] < 2.1 * 3.8);
    /* noise: bounded, no DC */
    {
        static int32_t nz[NB];
        double dc = 0;
        int32_t pk = 0;
        uint32_t s = 12345u;
        for (i = 0; i < NB; i++) {
            s = s * 1664525u + 1013904223u;
            nz[i] = (int32_t)(s >> 16) - 32768;
        }
        for (uint32_t m = 0; m < 3u; m++) {
            run(MV[m], 127, nz);
            for (i = FS; i < NB; i++) {
                dc += l_s[i];
                pk = abs(l_s[i]) > pk ? abs(l_s[i]) : pk;
            }
        }
        ck("full-scale noise: bounded (< 1.2 x the input) and no DC (< 0.5 % of it) in every mode", pk < 39000 && fabs(dc / (3.0 * (NB - FS))) < 160);
    }
    /* a switch change glides: no step past the sine's own slope (20000 * 2 pi 100 / 44100 = 285 per sample) */
    {
        static int32_t b1[NB];
        int32_t jmp = 0;
        run(0, 64, in_s);
        memcpy(b1, l_s, sizeof b1);
        {   /* I for 2 s, then I+II (and back at 3 s): the swing and rate change at once on the switch */
            static int32_t zero[CTL], wet[CTL];
            uint32_t k;
            memset(&fx, 0, sizeof fx);
            memset(cho_buf, 0, sizeof cho_buf);
            for (i = 0; i < NB; i += CTL) {
                song.g[G_CRATE] = (int16_t)(i < 2u * FS ? 0 : i < 3u * FS ? 127 : 0);
                song.g[G_CDEPTH] = 64;
                memset(zero, 0, sizeof zero);
                fx_buses(in_s + i, zero, zero, wet, CTL);
                for (k = 0; k < CTL; k++)
                    l_s[i + k] = wet[k] + cho_sd[k];
            }
        }
        for (i = FS / 2; i < NB; i++) {
            int32_t j = abs(l_s[i] - l_s[i - 1]);
            jmp = j > jmp ? j : jmp;
        }
        printf("chorus:   largest step over two switch changes: %d (the sine's slope 285)\n", jmp);
        ck("a switch change glides: no step past 1.5 x the sine's own slope", jmp < 430);
    }
    {   /* the page's labels and values */
        const param_desc_t *d = &GP[G_CRATE], *e = &GP[G_CDEPTH];
        char v[16];
        const char *u;
        ok = str_eq(d->label, "RATE") && str_eq(e->label, "DEPTH") && d->fmt == F_INT && d->min == 0 && d->max == 127;
        param_format(d, 0, v, &u);
        ok &= str_eq(v, "I");
        param_format(d, 64, v, &u);
        ok &= str_eq(v, "II");
        param_format(d, 127, v, &u);
        ok &= str_eq(v, "I+II");
        param_format(d, 40, v, &u);                       /* (a project saved before: CRT 40 is I) */
        ok &= str_eq(v, "I") && d->def == 64;
        ck("CHORUS page: RATE shows I / II / I+II over the range (a stored 40 is I), DEPTH as a percent", ok);
    }
    if (bad) {
        printf("CHORUS TEST FAILED (%d)\n", bad);
        return 1;
    }
    printf("chorus: all ok\n");
    return 0;
}
