/* SPDX-License-Identifier: GPL-3.0-only
 * Host tool: render a LEAD sound from a list of "id value" lines (what the editor's .sy1 importer sends, tools/sy1_ab.mjs) to
 * raw float32 mono at 44.1 kHz: the A side of the Synth1 A/B (the B side is fm1-sy1's s1render).
 *
 *   sy1_render PARAMS.txt OUT.f32 [note=60] [vel=100] [hold_s=2] [tail_s=1]
 *
 * The track starts as an init LEAD (as tests/lead_test.c: the operator slots at 0, the sends off), the pairs are applied on top,
 * the key is held for hold_s and released for tail_s. The output is the track's mono bus (before the master chain), in the engine's integer units / 32768. */
#define main hostsim_main
#include "hostsim.c"
#undef main

static uint32_t eng_lead(void)
{
    uint32_t e;
    for (e = 0; e < NENGINES; e++)
        if (ENGINES[e] == &ENG_LEAD)
            return e;
    return 0;
}

int main(int argc, char **argv)
{
    static int32_t b[CTL];
    FILE *f, *out;
    int id, v, note = argc > 3 ? atoi(argv[3]) : 60, vel = argc > 4 ? atoi(argv[4]) : 100;
    double hold = argc > 5 ? atof(argv[5]) : 2.0, tail = argc > 6 ? atof(argv[6]) : 1.0;
    uint32_t i, nh, nt;
    if (argc < 3) {
        fprintf(stderr, "usage: sy1_render PARAMS.txt OUT.f32 [note] [vel] [hold_s] [tail_s]\n");
        return 2;
    }
    memset(trk, 0, sizeof trk);
    host_tracks_init();
    host_preset(&trk[0], eng_lead(), 0);
    for (i = P_FM1_ATK; i <= P_FM4_LEVEL; i++)
        trk[0].p[i] = 0;
    trk[0].p[P_DIST] = trk[0].p[P_CHOR] = trk[0].p[P_DLY] = trk[0].p[P_REV] = 0;
    trk[0].p[P_ED_FLT] = trk[0].p[P_ED_PIT] = 0;
    trk[0].p[P_VOICE] = V_POLY;
    f = fopen(argv[1], "r");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    while (fscanf(f, "%d %d", &id, &v) == 2)
        if (id >= 0 && id < P_COUNT)
            trk[0].p[id] = (int16_t)v;
    fclose(f);
    out = fopen(argv[2], "wb");
    trk_note_on(&trk[0], (uint8_t)note, (uint8_t)vel);
    nh = (uint32_t)(hold * FS) / CTL;
    nt = (uint32_t)(tail * FS) / CTL;
    for (i = 0; i < nh + nt; i++) {
        uint32_t k;
        if (i == nh)
            trk_note_off(&trk[0], (uint8_t)note);
        mod_begin(&trk[0]);                           /* the block as fx.c runs a part: the matrix, the voices, then the LFO's end (voice.c) */
        track_render(&trk[0], b, CTL);
#if FELUCCA_CPU2
        track_lfo_end(&trk[0]);
#endif
        for (k = 0; k < CTL; k++) {
            float s = (float)b[k] / 32768.0f;
            fwrite(&s, 4, 1, out);
        }
    }
    fclose(out);
    return 0;
}
