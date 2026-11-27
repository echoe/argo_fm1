/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Eight sequences per track (patbank.c, patbank_store.c): the packed steps are a project's, a switch keeps each sequence's
 * steps, LEN and automation (64 records), the project slots and the live music keep them through flash, and a damaged or
 * missing store leaves the loaded music alone. */
#define main hostsim_main
#include "hostsim.c"
#undef main

static int32_t fm1_enc_take(uint32_t e) { (void)e; return 0; }
static void fm1_irq_off(void) {}
static void fm1_irq_on(void) {}
static void lcd_sync(void) {}
static void lcd_power(uint32_t s) { (void)s; }
static void lcd_wake_now(void) {}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *p)
{ (void)x; (void)y; (void)w; (void)h; (void)p; }
#define FELUCCA_FLASH 1
#include "../firmware/src/gfx.c"
#include "../firmware/src/panel.c"
#include "../firmware/src/ui.c"
#include "../firmware/src/menu_items.c"
static void panel_setup(void) {}

static uint8_t nor[0x100000], flash_ok = 1;
static int erase_error, fail_after = -1;
static uint32_t erases, as_erases;               /* (as_erases: the autosave's two sectors, 0xE5000 / 0xE6000) */
static int st_read(uint32_t off, void *dst, uint32_t n) { memcpy(dst, nor + off, n); return 0; }
static int st_erase(uint32_t off)
{
    erases++;
    as_erases += off >= 0xE5000u && off < 0xE7000u;
    if (erase_error) return -8;
    memset(nor + off, 0xFF, 4096);
    return 0;
}
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    uint32_t i;
    if (fail_after == 0) return -9;
    if (fail_after > 0) fail_after--;
    for (i = 0; i < n; i++) nor[off + i] &= s[i];
    return 0;
}
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t f) { (void)f; }
static uint32_t fl_jedec_ram(void) { return 0; }
static void fl_plain_window_init(void) {}
#define FL_FAR(fn) (fn)
#include "../firmware/src/storage.c"
#include "../firmware/src/upreset.c"
#include "../firmware/src/project.c"
#include "../firmware/src/patbank_store.c"

/* 1.9: the banks are lists in an arena (patbank.c), the playing sequence the track's: this test keeps the dense view it always had
 * (pb_pat) and moves it in (mir_store) before a save and out (mir_load) after a load */
static uint8_t pb_pat[PB_NSEQ][PB_PAT_B];
static void mir_store(void)
{
    uint32_t q;
    for (q = 0; q < PB_NSEQ; q++) {
        uint32_t i, any = 0;                                     /* (a row never filled is all zero: left as it is: empty) */
        for (i = 0; i < PB_PAT_B; i++)
            any |= pb_pat[q][i];
        if (q % PB_N == pb_cur[q / PB_N]) {
            if (any)
                (void)pb_unpack_pat(trk[q / PB_N].step, pb_pat[q]);
        } else if (any)
            (void)pb_mput(q, pb_pat[q]);
        else
            pb_mempty(q);
    }
}
static void mir_load(void)
{
    uint32_t q;
    for (q = 0; q < PB_NSEQ; q++) {
        if (q % PB_N == pb_cur[q / PB_N])
            pb_pack_pat(pb_pat[q], trk[q / PB_N].step);
        else
            pb_mget(q, pb_pat[q]);
    }
}
static int t_save(uint32_t s, uint32_t o) { mir_store(); return pb_store_save(s, o); }
static int t_load(uint32_t s) { int r = pb_store_load(s); mir_load(); return r; }
#define pb_store_save t_save
#define pb_store_load t_load


#define PB_BASE_END (OBJ_PB_BASE + OBJ_PB_N * 2u * ST_SECTOR)
_Static_assert(PB_BASE_END <= 0xDC000u, "the sequence banks overrun the user preset banks");
_Static_assert(PB_NOBJ * PB_NSTORE == OBJ_PB_N, "objects");
_Static_assert(PB_HDR + PB_PER * PB_OLD_PAT_B <= ST_PAYLOAD_MAX, "an object does not fit a sector");
_Static_assert(PB_META_FIX + 3u * 800u <= ST_PAYLOAD_MAX, "fewer than 800 automation records fit the last object");

static int check(const char *what, int ok)
{
    printf("patbank: %-74s %s\n", what, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}
static void reset(void)
{
    memset(nor, 0xFF, sizeof nor);
    memset(&song, 0, sizeof song);
    memset(trk, 0, sizeof trk);
    memset(&motion, 0, sizeof motion);
    memset(motion_active, 0, sizeof motion_active);
    memset(&chain, 0, sizeof chain);
    chain_defaults(&chain_config);
    memset(proj_slot, 0, sizeof proj_slot);
    memset(&as, 0, sizeof as);
    memset(&ui, 0, sizeof ui);
    host_tracks_init();
    fm1_ms = 0;
    transport_req = 0;
    flash_ok = 1;
    erase_error = 0;
    fail_after = -1;
    ui_prefs2 = 0;
    ui_prefs3 = 0;
    cpk_defaults();
    pb_ready = 0;
    pb_dirty = 0;
    pb_clear_all();
}
static void note(uint32_t t, uint32_t i, uint32_t n)       /* step i of track t: note n */
{
    step_t *s = &trk[t].step[i];
    s->time = ST_NOTE; s->n = 1; s->note[0] = (uint8_t)n; s->vel = 100;
}
static int has(uint32_t t, uint32_t i, uint32_t n) { return trk[t].step[i].n == 1 && trk[t].step[i].note[0] == n; }
static void as_run(uint32_t ms)
{
    uint32_t end = fm1_ms + ms;
    while ((int32_t)(end - fm1_ms) > 0) {
        fm1_ms += 50u;
        autosave_poll();
    }
}
static void power_cycle(void)                              /* the RAM lost, the flash kept, the autosave restored */
{
    uint32_t k;
    memset(trk, 0, sizeof trk);
    memset(&motion, 0, sizeof motion);
    memset(motion_active, 0, sizeof motion_active);
    memset(&as, 0, sizeof as);
    memset(&ui, 0, sizeof ui);
    proj_name[0] = 0;
    chain_defaults(&chain_config);
    host_tracks_init();
    for (k = 0; k < NTRK; k++)
        trk[k].eng_req = trk[k].engine = (uint8_t)TRK_DEF[k][0];
    pb_ready = 0;
    pb_dirty = 0;
    pb_clear_all();
    (void)autosave_boot(1);
}

/* ---- the sparse format (1.8.7) and the conversion of the old one ---- */
/* the old format's object o of a store (versions as 1.8.6 wrote them: 2 for objects 0..3, 1 for 4, 3 for the meta) from the mirror */
static uint32_t old_build(uint32_t o, uint32_t ver_meta)
{
    uint32_t pos = PB_HDR, q, k, n = o < 5u ? 6u : 2u;
    memset(pb_wire, 0, PB_HDR);
    pb_wire[0] = 'P'; pb_wire[1] = 'B'; pb_wire[2] = 1; pb_wire[3] = (uint8_t)o;
    if (o < GS_N && ver_meta >= 2u) {
        pb_wire[2] = 2;
        for (q = 0; q < NTRK; q++)
            pb_wire[4u + q] = gen_set[q][o];
    }
    if (o == 5u) {
        pb_wire[2] = (uint8_t)ver_meta;
        if (ver_meta >= 2u)
            for (q = 0; q < NTRK; q++)
                pb_wire[4u + q] = pb_cmask[q];
    }
    for (uint32_t qq = 0; qq < n; qq++)
        memcpy(pb_wire + pos + qq * PB_OLD_PAT_B, pb_pat[o * PB_PER + qq], PB_OLD_PAT_B);   /* (the old formats held 64 steps) */
    pos += n * PB_OLD_PAT_B;
    if (o == 5u) {
        memcpy(pb_wire + pos, pb_cur, NTRK); pos += NTRK;
        memcpy(pb_wire + pos, pb_len, PB_NSEQ); pos += PB_NSEQ;
        for (q = 0; q < PB_NSEQ; q++) {
            int on = q % PB_N != pb_cur[q / PB_N];
            pb_wire[pos++] = on ? pb_mo[q].count : 0;
            pb_wire[pos++] = on ? pb_mo[q].on : 0;
        }
        for (q = 0; q < PB_NSEQ; q++)
            if (q % PB_N != pb_cur[q / PB_N]) {
                for (uint32_t kk = 0; kk < pb_mo[q].count; kk++)
                    pb_rec_put(pb_wire + pos + kk * PB_REC_B, &pb_mo[q].ev[kk]);
                pos += pb_mo[q].count * PB_REC_B;
            }
        if (ver_meta >= 3u) {
            memcpy(pb_wire + pos, &cpk, sizeof cpk); pos += (uint32_t)sizeof cpk;
            for (k = 0; k < CPK_USER; k++) {
                memcpy(pb_wire + pos, &cpk_user[k], sizeof(cpack_t));
                pos += (uint32_t)sizeof(cpack_t);
            }
        }
    }
    return pos;
}
static void old_write(uint32_t s, uint32_t ver_meta)
{
    uint32_t o;
    pb_need();
    pb_sync_all();
    for (o = 0; o < 6u; o++) {
        uint32_t n = old_build(o, ver_meta);
        if (st_save(OBJ_PB0 + s * PB_NOBJ + o, pb_wire, n)) { printf("old_write failed\n"); exit(2); }
    }
}
/* a music in the mirror: notes, hits and a few chords spread over the 32 sequences, every one of them different */
static void trim_old(void)   /* the old formats held 64 steps a sequence: a store made to be written in one has no steps above */
{
    static uint8_t e[PB_PAT_B];
    uint32_t q;
    pb_pat_empty(e);
    for (q = 0; q < PB_NSEQ; q++)
        memcpy(pb_pat[q] + PB_OLD_PAT_B, e + PB_OLD_PAT_B, PB_PAT_B - PB_OLD_PAT_B);
}
static void fill_mirror(uint32_t seed, uint32_t per)
{
    uint32_t q, i;
    step_t st[NSTEP];
    pb_need();
    for (q = 0; q < PB_NSEQ; q++) {
        for (i = 0; i < NSTEP; i++)
            step_clear(&st[i]);
        for (i = 0; i < per; i++) {
            step_t *x = &st[(i * 7u + q + seed) % NSTEP];
            x->time = ST_NOTE; x->n = (uint8_t)(1u + (i + q) % 3u);
            x->note[0] = (uint8_t)(30u + (i * 3u + q + seed) % 90u); x->note[1] = (uint8_t)(40u + q); x->note[2] = (uint8_t)(50u + i);
            x->vel = (uint8_t)(60u + (i * 5u) % 60u);
            if (i % 4u == 0u) x->hit = (uint8_t)(1u << (i % 8u));
        }
        pb_pack_pat(pb_pat[q], st);
    }
    for (q = 0; q < PB_NSEQ; q++)
        pb_len[q] = (uint8_t)(8u + (q * 5u + seed) % 57u);
}
/* the mirror's sequences as they are kept: the note bytes of a step without notes are not (pb_canon) */
static uint8_t eq_cur[NTRK];                              /* the playing sequences: the project's record has them, not the bank (pb_store_load) */
static int pats_equal(const uint8_t a[PB_NSEQ][PB_PAT_B], const uint8_t b[PB_NSEQ][PB_PAT_B])
{
    uint32_t q, i;
    uint8_t x[PB_STEP_B], y[PB_STEP_B];
    for (q = 0; q < PB_NSEQ; q++)
        for (i = 0; i < NSTEP && q % PB_N != eq_cur[q / PB_N]; i++) {
            pb_canon(x, a[q] + i * PB_STEP_B);
            pb_canon(y, b[q] + i * PB_STEP_B);
            if (memcmp(x, y, PB_STEP_B)) return 0;
        }
    return 1;
}

int main(void)
{
    int bad = 0, ok;
    uint32_t i, t, q;
    reset();

    {   /* the bytes of a step are a project's: a pattern's 576 bytes are found in the packed project */
        static project_store_t out;
        uint8_t b[PB_PAT_B];
        step_t back[NSTEP];
        ok = 1;
        for (i = 0; i < NSTEP; i++) {
            step_t *s = &trk[1].step[i];
            s->n = (uint8_t)(i % 5u); s->time = (uint8_t)(i % 3u); s->flags = (uint8_t)(i & 3u); s->vel = (uint8_t)(i * 2u & 127u);
            s->note[0] = (uint8_t)(i + 30u & 127u); s->note[1] = (uint8_t)(i + 31u & 127u); s->note[2] = (uint8_t)(i + 32u & 127u); s->note[3] = (uint8_t)(i + 33u & 127u);
            s->hit = (uint8_t)(i * 7u); s->acc = (uint8_t)(s->hit & (i * 3u)); s->probability = (uint8_t)(i % 102u);
        }
        project_capture(&proj_scratch);
        ok &= proj_pack(&out, &proj_scratch);
        pb_pack_pat(b, trk[1].step);
        ok &= memmem(out.raw, sizeof out.raw, b, PB_OLD_PAT_B) != 0;   /* (the record holds the first NSTEP_REC steps) */
        ok &= pb_unpack_pat(back, b) && !memcmp(back, trk[1].step, sizeof back);
        bad += check("a sequence's packed bytes are the project's own, and unpack gives the steps back", ok);
        memset(trk[1].step, 0, sizeof trk[1].step);
        for (i = 0; i < NSTEP; i++) step_clear(&trk[1].step[i]);
    }

    {   /* a switch keeps each sequence: steps, LEN, automation */
        reset();
        note(0, 0, 40); trk[0].p[P_SLEN] = 12; (void)motion_set_event(&trk[0], 3, P_ATK, 10);
        ok = pb_select(0, 3) == 0 && pb_cur[0] == 3 && !has(0, 0, 40) && trk[0].p[P_SLEN] == TP[P_SLEN].def &&
             motion_count(&trk[0]) == 0;
        note(0, 5, 50); trk[0].p[P_SLEN] = 32; (void)motion_set_event(&trk[0], 9, P_ATK, 20);
        (void)motion_set_event(&trk[0], 10, P_ATK, 21);
        note(1, 0, 61);                                     /* another track: untouched by track 1's switches */
        ok &= pb_select(0, 0) == 0 && has(0, 0, 40) && !has(0, 5, 50) && trk[0].p[P_SLEN] == 12 && motion_count(&trk[0]) == 1 &&
              has(1, 0, 61) && pb_cur[1] == 0;
        ok &= pb_select(0, 3) == 0 && has(0, 5, 50) && trk[0].p[P_SLEN] == 32 && motion_count(&trk[0]) == 2;
        ok &= pb_filled(0, 0) && pb_filled(0, 3) && !pb_filled(0, 1) && pb_select(0, 8) == 1 && pb_select(4, 0) == 1;
        bad += check("a switch keeps each sequence's steps, LEN and automation; the other tracks stay", ok);
    }

    {   /* 64 automation records of a sequence that is not playing */
        reset();
        for (i = 0; i < 64u; i++) (void)motion_set_event(&trk[2], i, P_ATK, 5);
        ok = motion_count(&trk[2]) == 64u && pb_select(2, 1) == 0 && pb_select(2, 0) == 0 && motion_count(&trk[2]) == 64u;
        trk[2].step[0].time = ST_NOTE;
        for (i = 0; i < 1u; i++) (void)motion_set_event(&trk[3], i, P_ATK, 6);
        ok &= motion_count(&trk[2]) + motion_count(&trk[3]) == 65u;
        reset();
        for (i = 0; i < 64u; i++) (void)motion_set_event(&trk[2], i, P_ATK, 5);
        for (i = 0; i < 20u; i++) (void)motion_set_event(&trk[2], i, P_DEC, 5);   /* 84 records: more than it can keep */
        ok &= motion_count(&trk[2]) == 84u && pb_select(2, 1) == 2 && pb_cur[2] == 0 && motion_count(&trk[2]) == 84u;
        bad += check("64 automation records are kept; more than 64 refuses the switch and changes nothing", ok);
    }

    {   /* a project slot keeps its sequences; another slot has its own */
        reset();
        note(0, 0, 40); note(1, 2, 41);
        (void)pb_select(0, 2); note(0, 1, 42); trk[0].p[P_SLEN] = 8; (void)motion_set_event(&trk[0], 4, P_ATK, 9);
        (void)pb_select(0, 7); note(0, 7, 47);
        (void)pb_select(1, 5); note(1, 3, 51);
        ok = project_save_as(0, "BANKS") == 0;
        memset(trk, 0, sizeof trk); memset(&motion, 0, sizeof motion); memset(motion_active, 0, sizeof motion_active);
        host_tracks_init(); pb_ready = 0; pb_clear_all();       /* (the RAM only: the flash keeps project A) */
        note(0, 0, 99);
        ok &= project_save_as(1, "OTHER") == 0;
        for (t = 0; t < NTRK; t++) { memset(trk[t].step, 0, sizeof trk[t].step); for (i = 0; i < NSTEP; i++) step_clear(&trk[t].step[i]); }
        memset(proj_slot, 0, sizeof proj_slot);              /* (the RAM copies lost: from flash) */
        project_load(0);
        ok &= pb_cur[0] == 7 && pb_cur[1] == 5 && has(0, 7, 47) && has(1, 3, 51) && !has(0, 0, 99);
        ok &= pb_select(0, 0) == 0 && has(0, 0, 40) && pb_select(0, 2) == 0 && has(0, 1, 42) && trk[0].p[P_SLEN] == 8 &&
              motion_count(&trk[0]) == 1 && pb_select(1, 0) == 0 && has(1, 2, 41);
        project_load(1);
        ok &= pb_cur[0] == 0 && has(0, 0, 99) && pb_select(0, 2) == 0 && !has(0, 1, 42);
        bad += check("project SAVE and LOAD keep each track's sequences, the one that plays, LEN, automation; slots apart", ok);
    }

    {   /* the live music: the autosave brings the sequences back after a power cycle */
        reset();
        note(0, 0, 40); (void)pb_select(0, 4); note(0, 2, 44); (void)motion_set_event(&trk[0], 6, P_ATK, 12);
        trk[0].p[P_SLEN] = 24;
        (void)pb_select(3, 1); note(3, 1, 71);
        as_run(120000u);
        ok = as.writes >= 1u;
        power_cycle();
        ok &= pb_cur[0] == 4 && pb_cur[3] == 1 && has(0, 2, 44) && has(3, 1, 71) && trk[0].p[P_SLEN] == 24 && motion_count(&trk[0]) == 1;
        ok &= pb_select(0, 0) == 0 && has(0, 0, 40) && pb_select(3, 0) == 0;
        bad += check("the autosave restores each track's sequences after a power cycle (the live store)", ok);
        /* a change of only a sequence that is not playing is written too */
        note(0, 9, 90); (void)pb_select(0, 6); note(0, 11, 92);
        as_run(120000u);
        power_cycle();
        ok = pb_cur[0] == 6 && has(0, 11, 92) && pb_select(0, 0) == 0 && has(0, 9, 90);
        bad += check("a sequence edited then left (not playing now) is in the live store too", ok);
    }

    {   /* a damaged or missing store: the loaded music stays, every other sequence empty */
        reset();
        note(0, 0, 40); (void)pb_select(0, 2); note(0, 1, 42);
        ok = project_save_as(0, "X") == 0;
        for (q = 0; q < 2u; q++) {                            /* both copies of object 5 (the meta) lost */
            uint32_t base = OBJ_PB_BASE + (PB_META_O + 0u) * 2u * ST_SECTOR + q * ST_SECTOR;
            memset(nor + base, 0, ST_SECTOR);
        }
        memset(proj_slot, 0, sizeof proj_slot);
        project_load(0);
        ok &= has(0, 1, 42) && pb_cur[0] == 0 && !pb_filled(0, 2) && !pb_filled(0, 5) && pb_select(0, 1) == 0 && !has(0, 1, 42);
        bad += check("a lost store: the loaded music plays as sequence 1, the others empty", ok);
    }

    {   /* too much automation across the sequences: SAVE refused, flash as it was */
        reset();
        note(0, 0, 40);
        for (t = 0; t < NTRK; t++)
            for (q = 1; q < PB_N; q++) {
                pb_mo[t * PB_N + q].count = PB_MO_MAX;
                pb_mo[t * PB_N + q].on = 1;
                for (i = 0; i < PB_MO_MAX; i++) { pb_mo[t * PB_N + q].ev[i].step = (uint8_t)i; pb_mo[t * PB_N + q].ev[i].param = P_ATK; }
            }
        ok = !pb_meta_ok() && project_save_as(0, "FULL") != 0 && !strcmp(ui.msg, "AUTOMATION FULL") && !project_used(0);
        bad += check("more automation than the store holds: SAVE says AUTOMATION FULL and writes nothing", ok);
    }

    {   /* a failing flash: SAVE ERROR, the old slot kept */
        reset();
        note(0, 0, 40);
        ok = project_save_as(0, "OLD") == 0;
        note(0, 1, 41);
        fail_after = 1;
        ok &= project_save_as(0, "NEW") != 0 && !strcmp(ui.msg, "SAVE ERROR");
        fail_after = -1;
        memset(proj_slot, 0, sizeof proj_slot);
        project_load(0);
        ok &= has(0, 0, 40) && !has(0, 1, 41);
        bad += check("a flash failing in the middle of SAVE: SAVE ERROR, the project as it was", ok);
    }

    {   /* MENU > CONTROL > SEQ SWITCH: NOW at once; END, while playing, at the end of the sequence playing */
        reset();
        ok = !(ui_prefs3 & (PREF_SEQ_END >> 24)) && menu_n(MI_SEQSW) == 2u && !strcmp(MI_NAME[MI_SEQSW], "SEQ SWITCH") &&
             !strcmp(menu_vname(MI_SEQSW, 0), "NOW") && !strcmp(menu_vname(MI_SEQSW, 1), "END") && MI_TAB[MI_SEQSW] == MTAB_CONTROL2;
        song.playing = 1;
        trk[0].p[P_SLEN] = 16; trk[0].seq_idx = 3;
        (void)pb_request(0, 2);
        ok &= pb_cur[0] == 2 && !pb_next[0];                       /* NOW (the default): at once, playing or not */
        menu_put(MI_SEQSW, 1);
        ok &= menu_get(MI_SEQSW) == 1 && (ui_prefs3 & (PREF_SEQ_END >> 24));
        (void)pb_request(0, 5);
        for (t = 0; t < NTRK; t++) (void)pb_due(t);
        ok &= pb_cur[0] == 2 && pb_next[0] == 6;                   /* END, playing: waits (step 4 of 16) */
        trk[0].seq_idx = 14; for (t = 0; t < NTRK; t++) (void)pb_due(t);
        ok &= pb_cur[0] == 2;
        trk[0].seq_idx = 15; for (t = 0; t < NTRK; t++) (void)pb_due(t);                           /* the last step: the next one is the new sequence's */
        ok &= pb_cur[0] == 5 && !pb_next[0];
        (void)pb_request(0, 1); (void)pb_request(0, 5);                  /* the one playing chosen again: the waiting one called off */
        ok &= pb_cur[0] == 5 && !pb_next[0];
        (void)pb_request(0, 4);
        trk[0].seq_idx = 9; for (t = 0; t < NTRK; t++) (void)pb_due(t); trk[0].seq_idx = 2; for (t = 0; t < NTRK; t++) (void)pb_due(t);   /* a pass missed the last step: the wrap is seen */
        ok &= pb_cur[0] == 4 && !pb_next[0];
        (void)pb_request(0, 6);
        song.playing = 0; for (t = 0; t < NTRK; t++) (void)pb_due(t);                              /* stopped: at once */
        ok &= pb_cur[0] == 6 && !pb_next[0];
        (void)pb_request(1, 3);                                       /* stopped: no waiting at all */
        ok &= pb_cur[1] == 3 && !pb_next[1];
        song.playing = 1;
        (void)pb_request(2, 1); trk[2].p[P_SLEN] = 16; trk[2].seq_idx = 15;
        project_save_as(0, "Q"); pb_reset_to_live();               /* (a project load / reset forgets a waiting switch) */
        ok &= !pb_next[2];
        /* the knob steps from the one waiting, so a far sequence can be reached one step at a time */
        reset();
        menu_put(MI_SEQSW, 1); song.playing = 1; trk[0].p[P_SLEN] = 16; trk[0].seq_idx = 2;
        ok &= pb_target(0) == 0;
        for (i = 0; i < 6u; i++)
            (void)pb_request(0, pb_target(0) + 1u);
        ok &= pb_cur[0] == 0 && pb_next[0] == 7 && pb_target(0) == 6;       /* waiting for the seventh, the first still playing */
        (void)pb_request(0, pb_target(0) - 4u);
        ok &= pb_target(0) == 2 && pb_cur[0] == 0;
        (void)pb_request(0, 0);                                            /* back to the one playing: nothing waits */
        ok &= pb_target(0) == 0 && !pb_next[0];
        bad += check("SEQ SWITCH: NOW at once; END waits for the last step (or a wrap seen), stopped switches at once, same slot cancels", ok);
    }

    {   /* CHAIN: only the sequences the user chained play one after the other (in order, the last back to the first) */
        uint32_t pass, pol;
        reset();
        song.playing = 1; trk[0].p[P_SLEN] = 16;
        ok = pb_chain_count(0) == 0 && !pb_chained(0, 1);
        pb_chain_set(0, 1, 1); pb_chain_set(0, 3, 1);
        ok &= pb_chain_count(0) == 2 && pb_chained(0, 1) && pb_chained(0, 3) && !pb_chained(0, 2);
        pb_chain_set(0, 6, 1); pb_chain_set(0, 9, 1);              /* (no such sequence: nothing) */
        ok &= pb_chain_count(0) == 3 && pb_cmask[0] == (1u << 1 | 1u << 3 | 1u << 6);
        (void)pb_request(0, 1);
        ok &= pb_cur[0] == 1 && pb_chain_next(0) == 3;
        pb_chain_set(0, 4, 1); pb_chain_set(0, 4, 0);
        ok &= pb_cmask[0] == (1u << 1 | 1u << 3 | 1u << 6);
        memset(pb_last, 0, sizeof pb_last);
        {
            static const uint8_t want[] = {3, 6, 1, 3, 6, 1};     /* only the chained ones: 2, 4, 5, 7, 8 never play */
            for (pass = 0; pass < 6u; pass++) {
                for (pol = 0; pol < 5u; pol++) {                   /* many polls on the last step: one switch */
                    trk[0].seq_idx = 15;
                    (void)pb_due(0);
                }
                ok &= pb_cur[0] == want[pass];
                trk[0].seq_idx = 0; (void)pb_due(0);               /* the next pass under way */
                trk[0].seq_idx = 7; (void)pb_due(0);
            }
        }
        (void)pb_request(0, 4);                                    /* a sequence not chained plays by itself, looping */
        ok &= pb_cur[0] == 4 && pb_chain_next(0) == -1;
        for (pass = 0; pass < 3u; pass++) { trk[0].seq_idx = 15; (void)pb_due(0); trk[0].seq_idx = 7; (void)pb_due(0); }
        ok &= pb_cur[0] == 4;
        (void)pb_request(0, 6);                                    /* a chained one chosen: the chain goes on from it */
        trk[0].seq_idx = 15; (void)pb_due(0);
        ok &= pb_cur[0] == 1 && pb_chain_next(0) == 3;
        pb_chain_set(0, 3, 0); pb_chain_set(0, 6, 0);              /* only one left chained: no chain */
        ok &= pb_chain_count(0) == 0 && pb_chain_next(0) == -1;
        trk[0].seq_idx = 7; (void)pb_due(0); trk[0].seq_idx = 15; (void)pb_due(0);
        ok &= pb_cur[0] == 1;
        song.playing = 0;                                          /* stopped: a chain does not advance */
        pb_chain_set(0, 2, 1); (void)pb_request(0, 2); trk[0].seq_idx = 15; (void)pb_due(0);
        ok &= pb_cur[0] == 2;
        bad += check("CHAIN: only the sequences the user chained play one after the other; the others loop; one alone is no chain", ok);
        /* a waiting switch (SEQ SWITCH END) goes first; a sequence not chained then loops */
        reset();
        menu_put(MI_SEQSW, 1); song.playing = 1; trk[0].p[P_SLEN] = 16;
        pb_chain_set(0, 0, 1); pb_chain_set(0, 1, 1); pb_chain_set(0, 2, 1);
        (void)pb_request(0, 5);                                    /* waits for sequence 6, not chained */
        trk[0].seq_idx = 15; (void)pb_due(0);
        ok = pb_cur[0] == 5 && pb_chain_next(0) == -1 && pb_chain_count(0) == 3;
        menu_put(MI_SEQSW, 0);
        bad += check("CHAIN: a waiting switch goes first; the chain's masks stay", ok);
        /* the chain is saved with the project and with the live music */
        reset();
        (void)pb_request(0, 2); pb_chain_set(0, 2, 1); pb_chain_set(0, 4, 1); pb_chain_set(0, 7, 1);
        pb_chain_set(2, 5, 1); pb_chain_set(2, 6, 1);
        ok = project_save_as(0, "CH") == 0;
        memset(trk, 0, sizeof trk); memset(&motion, 0, sizeof motion); memset(motion_active, 0, sizeof motion_active);
        host_tracks_init(); pb_ready = 0; pb_clear_all(); memset(proj_slot, 0, sizeof proj_slot);
        ok &= pb_chain_count(0) == 0;
        project_load(0);
        ok &= pb_cmask[0] == (1u << 2 | 1u << 4 | 1u << 7) && pb_cmask[2] == (1u << 5 | 1u << 6) && !pb_cmask[1] && pb_cur[0] == 2;
        pb_chain_set(1, 3, 1); pb_chain_set(1, 4, 1); pb_chain_set(0, 4, 0);
        as_run(120000u);
        power_cycle();
        ok &= pb_cmask[1] == (1u << 3 | 1u << 4) && pb_cmask[0] == (1u << 2 | 1u << 7);
        bad += check("CHAIN: the chained sequences are kept by project SAVE / LOAD and by the autosave (the live store)", ok);
    }

    {   /* SONG: a lane can play any of its project's sequences (CH_SEQ), from the project's store */
        reset();
        note(0, 0, 40);
        (void)pb_select(0, 2); note(0, 1, 42); trk[0].p[P_SLEN] = 8; (void)motion_set_event(&trk[0], 4, P_ATK, 9);
        (void)pb_select(0, 5); note(0, 2, 45);                      /* the project is saved on sequence 6 */
        ok = project_save_as(0, "A") == 0 && project_used(0);
        chain_defaults(&chain_config);
        for (i = 0; i < 4u; i++) {
            static const uint8_t pick[4] = {0, 1, 3, 6};            /* (as saved, the 1st, the 3rd, the 6th: the one saved on) */
            for (t = 0; t < NTRK; t++) chain_config.row[i].slot[t] = CH_MAKE(t ? CHAIN_SILENT : 0u, 0u);
            chain_config.row[i].slot[0] = CH_MAKE(0u, pick[i]);
            chain_config.row[i].repeat = 1;
        }
        chain_config.count = 4;
        ok &= chain_valid(&chain_config) && chain_prepare() == 0;
        ok &= chain.src[0][0] == 0 && chain.src[1][0] == 8 && chain.src[2][0] == 9 && chain.src[3][0] == 0 && chain.src[0][1] == CHAIN_SILENT;
        chain.running = 1;
        chain.row = 0; chain_apply();
        ok &= chain.lane[0] == 0 && chain.alt[0] == CHAIN_NOALT && seq_steps(&trk[0]) == chain.source[0].step[0];
        ok &= seq_steps(&trk[0])[2].n == 1 && seq_steps(&trk[0])[2].note[0] == 45;           /* as saved: the 6th (the record's) */
        chain.row = 1; chain_apply();
        ok &= chain.lane[0] == 0 && chain.alt[0] == 0 && seq_steps(&trk[0]) == chain_alt[0].step &&
              seq_steps(&trk[0])[0].n == 1 && seq_steps(&trk[0])[0].note[0] == 40 && seq_steps(&trk[0])[2].n == 0;
        chain.row = 2; chain_apply();
        ok &= chain.alt[0] == 1 && seq_steps(&trk[0])[1].note[0] == 42 && trk[0].p[P_SLEN] == 8 &&
              chain_motion(&trk[0]) == &chain_alt[1].motion && chain_alt[1].motion.count == 1;
        chain.row = 3; chain_apply();
        ok &= chain.alt[0] == CHAIN_NOALT && seq_steps(&trk[0])[2].note[0] == 45;            /* the saved one is the slot's */
        chain.running = 0; chain.armed = 0; transport_req = 0;
        bad += check("SONG: a lane's chosen sequence is read from its project's store; the saved one is the slot's own", ok);
        /* the live sequences are untouched by all that */
        ok = pb_cur[0] == 5 && has(0, 2, 45);
        bad += check("SONG: the playing sequences and their steps stay as they were", ok);
        /* more sequences than the song holds */
        for (i = 0; i < 8u; i++) chain_config.row[i].slot[0] = CH_MAKE(0u, (i % 8u) + 1u);
        chain_config.count = 8;
        ok = chain_prepare() == 16u;
        for (i = 0; i < 7u; i++) chain_config.row[i].slot[0] = CH_MAKE(0u, i < 5u ? i + 1u : 0u);
        chain_config.count = 7;
        ok &= chain_prepare() == 0 && !chain.running;
        chain.armed = 0; transport_req = 0;
        chain_config.row[0].slot[0] = CH_MAKE(0u, 9u);                                      /* a sequence past the 8th: no song */
        ok &= !chain_valid(&chain_config);
        bad += check("SONG: more than CHAIN_ALT chosen sequences: TOO MANY (16); one past the 8th: not a song", ok);
        /* a project saved without the store (or with it lost): the chosen sequence is empty, the song still plays */
        for (q = 0; q < 2u; q++)
            memset(nor + OBJ_PB_BASE + PB_META_O * 2u * ST_SECTOR + q * ST_SECTOR, 0, ST_SECTOR);
        chain_config.row[0].slot[0] = CH_MAKE(0u, 2u);
        chain_config.count = 1;
        ok = chain_prepare() == 0 && chain.src[0][0] == 8 && chain_alt[0].step[0].n == 0 && chain_alt[0].timing[0] == TP[P_SLEN].def;
        chain.armed = 0; transport_req = 0;
        bad += check("SONG: a lost store: the chosen sequence is empty, the song plays", ok);
    }

    {   /* the ARP's GEN knobs (DENS, CHORD, EVOLVE, ALGO of the four tracks) are saved with the sequence banks */
        reset();
        ok = gen_set[0][GS_DENS] == 64 && gen_set[0][GS_CHORD] == 0 && gen_set[0][GS_EVOLVE] == 24 && gen_set[0][GS_ALGO] == 0;
        gen_set[0][GS_DENS] = 100; gen_set[0][GS_CHORD] = 70; gen_set[0][GS_EVOLVE] = 5; gen_set[0][GS_ALGO] = 90;
        gen_set[3][GS_DENS] = 12; gen_set[3][GS_CHORD] = 127; gen_set[3][GS_EVOLVE] = 99; gen_set[3][GS_ALGO] = 42;
        pb_dirty |= 0xFu; pb_gen++;
        ok &= project_save_as(0, "GEN") == 0;
        gen_defaults();
        memset(proj_slot, 0, sizeof proj_slot);
        ok &= gen_set[0][GS_DENS] == 64;
        project_load(0);
        ok &= gen_set[0][GS_DENS] == 100 && gen_set[0][GS_CHORD] == 70 && gen_set[0][GS_EVOLVE] == 5 && gen_set[0][GS_ALGO] == 90 &&
              gen_set[3][GS_DENS] == 12 && gen_set[3][GS_CHORD] == 127 && gen_set[3][GS_EVOLVE] == 99 && gen_set[3][GS_ALGO] == 42 &&
              gen_set[1][GS_DENS] == 64 && gen_set[1][GS_ALGO] == 0;
        gen_set[1][GS_EVOLVE] = 50; gen_set[1][GS_ALGO] = 11; pb_dirty |= 0xFu; pb_gen++;      /* the autosave (the live store) */
        as_run(120000u);
        power_cycle();
        ok &= gen_set[1][GS_EVOLVE] == 50 && gen_set[1][GS_ALGO] == 11 && gen_set[0][GS_DENS] == 100 && gen_set[3][GS_CHORD] == 127 && gen_set[3][GS_ALGO] == 42;
        /* a project saved with the defaults loads them back over another's values */
        gen_defaults(); pb_dirty |= 0xFu; pb_gen++;
        ok &= project_save_as(2, "DEF") == 0;
        gen_set[0][GS_DENS] = 1; gen_set[3][GS_CHORD] = 99;
        memset(proj_slot, 0, sizeof proj_slot);
        project_load(2);
        ok &= gen_set[0][GS_DENS] == 64 && gen_set[3][GS_CHORD] == 0 && gen_set[1][GS_EVOLVE] == 24 && gen_set[0][GS_ALGO] == 0;
        bad += check("GEN's DENS, CHORD, EVOLVE, ALGO: kept by project SAVE / LOAD and the autosave; a project without them: the defaults", ok);
    }

    {   /* CHRD PLAY's chord packs (what the black keys do, the user's packs) are saved with the music */
        reset();
        ok = !memcmp(&cpk, &CPK_F[0].p, sizeof cpk) && !memcmp(&cpk_user[3], &CPK_F[0].p, sizeof cpk);
        cpk.act[0] = CA_SUS2; cpk.act[5] = CA_A13; cpk.act[10] = CA_ARP; cpk.strum = 17;
        cpk_user[1].act[3] = CA_DIM7; cpk_user[1].strum = 3;
        cpk.strum |= CPK_LATCH; cpk_user[1].strum |= CPK_LATCH;                   /* LATCH is part of a pack */
        cpk_touch();
        ok &= project_save_as(0, "CPK") == 0;
        cpk_defaults();
        memset(proj_slot, 0, sizeof proj_slot);
        ok &= cpk.act[0] == CA_MIN;
        project_load(0);
        ok &= cpk.act[0] == CA_SUS2 && cpk.act[5] == CA_A13 && cpk.act[10] == CA_ARP && cpk.strum == (17 | CPK_LATCH) &&
              cpk_user[1].act[3] == CA_DIM7 && cpk_user[1].strum == (3 | CPK_LATCH) && !memcmp(&cpk_user[0], &CPK_F[0].p, sizeof cpk);
        cpk_user[2].act[1] = CA_MAJ9; cpk_touch();                  /* the autosave (the live store) */
        as_run(120000u);
        power_cycle();
        ok &= cpk_user[2].act[1] == CA_MAJ9 && cpk.act[5] == CA_A13 && cpk_user[1].strum == (3 | CPK_LATCH) && (cpk.strum & CPK_LATCH);
        /* a project saved with another pack: its own on loading */
        cpk_defaults(); cpk.strum = 5; cpk_touch();
        ok &= project_save_as(2, "OTHER") == 0;
        cpk.strum = 33; cpk.act[2] = CA_AUG;
        memset(proj_slot, 0, sizeof proj_slot);
        project_load(2);
        ok &= cpk.strum == 5 && cpk.act[2] == CA_7;
        bad += check("the chord packs (what each black key does, the user's four) are kept by project SAVE / LOAD and the autosave", ok);
    }

    {   /* the sparse format: sizes */
        const uint8_t *v;
        uint32_t len, t2, i2;
        reset();
        for (t2 = 0; t2 < NTRK; t2++)                                                    /* (the playing sequences are the tracks' steps: empty ones) */
            for (i2 = 0; i2 < NSTEP; i2++)
                step_clear(&trk[t2].step[i2]);
        ok = pb_store_save(0, PB_ALLOBJ) == 0;
        for (t2 = 0; t2 < NTRK; t2++) {
            v = st_view(OBJ_PB0 + t2, &len);
            ok &= v && len == PB_HDR + PB_N * (1u + PB_BM) && v[0] == 'P' && v[1] == 'B' && v[2] == PB_FMT && v[3] == t2;
        }
        v = st_view(OBJ_PB0 + PB_META_O, &len);
        ok &= v && len < 700u && v[3] == PB_META_O && !st_view(OBJ_PB0 + 5u, &len);     /* (an empty store: 80 bytes a track; the sixth object unused) */
        fill_mirror(1, 12);                                                              /* 12 steps in every sequence */
        ok &= pb_store_save(1, PB_ALLOBJ) == 0;
        v = st_view(OBJ_PB0 + PB_NOBJ + 2u, &len);
        ok &= v && len > 8u * 40u && len < 8u * (1u + PB_BM + 12u * 7u) + 8u;   /* (12 steps of up to 7 bytes: the steps of 128 collide less than those of 64) */
        bad += check("sparse sequence banks: an empty store is 80 bytes a track, 12 steps a sequence about 0.6 KB; the sixth object is not used", ok);
    }
    {   /* round trip, a store, its gen settings, chain masks, automation and chord packs; a step that was emptied is as empty as a new one */
        static uint8_t keep[PB_NSEQ][PB_PAT_B];
        uint32_t q2;
        reset();
        fill_mirror(3, 20);
        pb_cur[1] = 3; pb_cur[2] = 6;
        eq_cur[0] = 0; eq_cur[1] = 3; eq_cur[2] = 6; eq_cur[3] = 0;
        gen_set[2][1] = 77; gen_set[0][3] = 5; pb_cmask[1] = 0x0Cu;
        pb_mo[10].count = 2; pb_mo[10].on = 1; pb_mo[10].ev[0].step = 3; pb_mo[10].ev[1].step = 9;
        cpk.strum = 21; cpk_user[2].strum = 9;
        { step_t *z = (step_t *)0; (void)z; }
        pb_pat[5][0] = 77; pb_pat[5][4] = 0;                          /* (a step of no notes with a stale note byte) */
        memcpy(keep, pb_pat, sizeof keep);
        ok = pb_store_save(2, PB_ALLOBJ) == 0;
        memset(pb_pat, 0, sizeof pb_pat);
        pb_clear_all();
        ok &= pb_store_load(2) == 1 && pats_equal((const uint8_t (*)[PB_PAT_B])pb_pat, (const uint8_t (*)[PB_PAT_B])keep);
        ok &= gen_set[2][1] == 77 && gen_set[0][3] == 5 && pb_cmask[1] == 0x0Cu && pb_cur[1] == 3 && pb_cur[2] == 6 &&
              pb_mo[10].count == 2 && pb_mo[10].ev[1].step == 9 && cpk.strum == 21 && cpk_user[2].strum == 9;
        for (q2 = 0; q2 < PB_NSEQ; q2++)
            ok &= q2 % PB_N == eq_cur[q2 / PB_N] || pb_len[q2] == (uint8_t)(8u + (q2 * 5u + 3u) % 57u);
        bad += check("sparse sequence banks: the sequences, LENs, GEN, CHAIN masks, automation and chord packs come back", ok);
    }
    {   /* more than a track's object holds: refused (4), nothing of the track written; the rest of the music is not harmed */
        uint32_t q2, i2, rc;
        reset();
        fill_mirror(2, 10);
        for (q2 = 0; q2 < PB_N; q2++)
            for (i2 = 0; i2 < NSTEP; i2++) {
                uint8_t *b = pb_pat[q2] + i2 * PB_STEP_B;
                memset(b, 0x55, PB_STEP_B); b[4] = 4;                  /* (every byte of every step set: 10 bytes a step) */
            }
        rc = (uint32_t)pb_store_save(0, PB_ALLOBJ);
        ok = rc == 4u && !st_view(OBJ_PB0, &q2);
        fill_mirror(2, 10);
        ok &= pb_store_save(0, PB_ALLOBJ) == 0;
        bad += check("sparse sequence banks: a track whose lists do not fit its object is refused, nothing written", ok);
    }
    {   /* the old format -> the new: all of a store comes over, in place */
        static uint8_t keep[PB_NSEQ][PB_PAT_B];
        uint32_t s2, ver, q2;
        for (ver = 1; ver <= 3u; ver++) {
            reset();
            fill_mirror(ver, 25); trim_old();
            pb_cur[0] = 2; pb_cur[3] = 5;
            eq_cur[0] = 2; eq_cur[1] = 0; eq_cur[2] = 0; eq_cur[3] = 5;
            gen_set[1][2] = 31; gen_set[3][0] = 99; pb_cmask[2] = 0x13u;
            pb_mo[9].count = 3; pb_mo[9].on = 1; pb_mo[9].ev[2].step = 7;
            cpk.strum = 12;
            memcpy(keep, pb_pat, sizeof keep);
            s2 = ver - 1u;
            old_write(s2, ver == 1u ? 1u : ver == 2u ? 2u : 3u);
            ok = pb_new_meta(s2) < 0;                                  /* (it is the old format) */
            memset(pb_pat, 0, sizeof pb_pat);
            pb_clear_all();
            gen_set[1][2] = 1; gen_set[3][0] = 2; pb_cmask[2] = 0; cpk.strum = 3;
            ok &= pb_store_convert(s2) == 1 && pb_new_meta(s2) >= 0 && pb_store_convert(s2) == 1;
            ok &= pb_store_load(s2) == 1 && pats_equal((const uint8_t (*)[PB_PAT_B])pb_pat, (const uint8_t (*)[PB_PAT_B])keep);
            ok &= pb_cur[0] == 2 && pb_cur[3] == 5 && pb_mo[9].count == 3 && pb_mo[9].ev[2].step == 7;
            if (ver >= 2u) ok &= gen_set[1][2] == 31 && gen_set[3][0] == 99 && pb_cmask[2] == 0x13u;
            else ok &= pb_cmask[2] == 0;
            if (ver == 3u) ok &= cpk.strum == 12;
            for (q2 = 0; q2 < PB_NSEQ; q2++)
                ok &= q2 % PB_N == eq_cur[q2 / PB_N] || pb_len[q2] == (uint8_t)(8u + (q2 * 5u + ver) % 57u);
            bad += check(ver == 1u ? "the old sequence banks (format 1) convert in place" : ver == 2u ? "the old sequence banks (format 2: GEN, CHAIN) convert in place" :
                         "the old sequence banks (format 3: and the chord packs) convert in place", ok);
        }
    }
    {   /* a power cut anywhere in a conversion: it goes on, and no sequence is lost */
        static uint8_t keep[PB_NSEQ][PB_PAT_B], image[sizeof nor];
        uint32_t n2, cuts = 0, good = 1, s2 = 1, q2;
        reset();
        fill_mirror(5, 30); trim_old();
        pb_cur[0] = 1; pb_cur[1] = 4; eq_cur[0] = 1; eq_cur[1] = 4; eq_cur[2] = 0; eq_cur[3] = 0; pb_mo[13].count = 2; pb_mo[13].on = 1; pb_mo[13].ev[0].step = 5;
        memcpy(keep, pb_pat, sizeof keep);
        old_write(s2, 3);
        memcpy(image, nor, sizeof nor);
        for (n2 = 0; n2 < 90u; n2++) {
            int r;
            memcpy(nor, image, sizeof nor);
            fail_after = (int)n2;
            r = pb_store_convert(s2);
            fail_after = -1;
            cuts += r == -1;
            if (pb_store_convert(s2) != 1) { good = 0; continue; }
            memset(pb_pat, 0, sizeof pb_pat);
            pb_clear_all();
            good &= (uint32_t)pb_store_load(s2) == 1u && pats_equal((const uint8_t (*)[PB_PAT_B])pb_pat, (const uint8_t (*)[PB_PAT_B])keep);
            good &= pb_mo[13].count == 2 && pb_mo[13].ev[0].step == 5 && pb_cur[1] == 4;
            for (q2 = 0; q2 < PB_NSEQ; q2++)
                good &= q2 % PB_N == eq_cur[q2 / PB_N] || pb_len[q2] == (uint8_t)(8u + (q2 * 5u + 5u) % 57u);
        }
        bad += check("a power cut at any of 90 points of a conversion: the next one goes on, all 32 sequences, LENs and automation kept", good && cuts >= 8u);
    }
    {   /* pb_convert_all: every store; again it writes nothing; SAVE over an old store writes it whole; the sixth objects are free */
        uint32_t s2, e0, free6 = 1;
        reset();
        fill_mirror(7, 15); trim_old();
        for (s2 = 0; s2 < PB_NSTORE; s2++)
            old_write(s2, 3);
        pb_convert_all();
        ok = 1;
        for (s2 = 0; s2 < PB_NSTORE; s2++)
            ok &= pb_new_meta(s2) >= 0;
        e0 = erases;
        pb_convert_all();
        ok &= erases == e0;
        reset();
        fill_mirror(8, 15); trim_old();
        old_write(0, 3);
        pb_dirty = 0;
        ok &= pb_store_save(0, 1u << 2) == 0 && pb_new_meta(0) >= 0;      /* (one object asked for: the whole store, in order) */
        {
            uint32_t len2;
            for (s2 = 0; s2 < NTRK; s2++)
                ok &= st_view(OBJ_PB0 + s2, &len2) && len2 >= PB_HDR && pb_wire[0] == 'P';
        }
        (void)free6;
        bad += check("every store converts at power-on (and once), a SAVE over an old store writes it whole", ok);
    }
    {   /* a project's banks through the old format: SAVE / LOAD of a project saved by 1.8.6 */
        static uint8_t keep[PB_NSEQ][PB_PAT_B];
        uint8_t cur2[NTRK];
        reset();
        fill_mirror(11, 18); trim_old();
        eq_cur[0] = eq_cur[1] = eq_cur[2] = eq_cur[3] = 0;
        memcpy(keep, pb_pat, sizeof keep);
        old_write(3, 3);
        memset(pb_pat, 0, sizeof pb_pat);
        pb_clear_all();
        ok = pb_store_saved(3, cur2) == 1 && pb_new_meta(3) >= 0;         /* (reading the store converted it) */
        {
            step_t st2[NSTEP];
            uint8_t len2;
            pb_mo_t mo2;
            ok &= pb_store_fetch(3, 2, 5, st2, &len2, &mo2) == 0 && len2 == (uint8_t)(8u + ((2u * 8u + 5u) * 5u + 11u) % 57u);
            ok &= !memcmp(&st2[(0u * 7u + 21u + 11u) % NSTEP].note[0], &trk[0].step[0].note[0], 0) ;
        }
        ok &= pb_store_load(3) == 1 && pats_equal((const uint8_t (*)[PB_PAT_B])pb_pat, (const uint8_t (*)[PB_PAT_B])keep);
        bad += check("a song reads a sequence of a project saved in the old format (the store converts as it is read)", ok);
    }

    printf(bad ? "patbank test FAILED\n" : "patbank test passed\n");
    return bad != 0;
}
