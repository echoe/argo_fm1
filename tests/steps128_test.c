/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* 1.9: sequences of 128 steps: kept by a project SAVE / LOAD and the autosave (the record holds 64 steps, the sequence bank the rest),
 * switched, and played to their last step. */
#define main hostsim_main
#include "hostsim.c"
#undef main

static int32_t fm1_enc_take(uint32_t e) { (void)e; return 0; }
static uint32_t irq_start_race, irq_races;
static void fm1_irq_off(void)
{
    if (irq_start_race) {                         /* audio serviced PLAY immediately before main masks it */
        irq_start_race = 0;
        events_block(CTL);
        irq_races++;
    }
}
static void fm1_irq_on(void) {}
static void lcd_sync(void) {}
static void lcd_power(uint32_t s) { (void)s; }   /* (MENU > SCREEN OFF: lcd.c) */
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

enum { ED_UI_STATE = 34, ED_UI_SET, ED_UI_PALETTES, ED_FAV_GET, ED_FAV_SET };
static uint8_t ed_rep[64];                        /* the reply's bytes (MENU_SET) */
static uint32_t ed_repn;
static void ed_b(uint32_t n) { if (ed_repn < sizeof ed_rep) ed_rep[ed_repn++] = (uint8_t)(n & 127u); }
static void ed_v(int32_t n) { ed_b((uint32_t)(n + 8192)); ed_b((uint32_t)(n + 8192) >> 7); }
static int32_t ed_rv(const uint8_t *a) { return (int32_t)(a[0] | (uint32_t)a[1] << 7) - 8192; }
static void ed_str(const char *s, uint32_t n) { (void)s; (void)n; }
#include "../firmware/src/editor_preferences.c"
#include "../firmware/src/editor_menu.c"

static int check(const char *what, int ok)
{
    printf("steps128: %-70s %s\n", what, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

static void reset(void)
{
    memset(nor, 0xFF, sizeof nor);
    memset(&song, 0, sizeof song);
    memset(trk, 0, sizeof trk);
    memset(&chain, 0, sizeof chain);
    chain_defaults(&chain_config);
    memset(proj_slot, 0, sizeof proj_slot);
    memset(up_bank, 0, sizeof up_bank);
    memset(&persist_saved, 0, sizeof persist_saved);
    memset(&settings, 0, sizeof settings);
    memset(&ui, 0, sizeof ui);
    memset(&favorites, 0, sizeof favorites);
    panel = PANEL_DEFAULT;
    host_tracks_init();
    fm1_ms = 0;
    transport_req = 0;
    persist_pending = 0;
    persist_retry_ms = 0;
    flash_ok = 1;
    erase_error = 0;
    fail_after = -1;
    up_gen = erases = 0;
    irq_start_race = irq_races = 0;
}


static void as_run(uint32_t ms)                          /* the main loop for ms (autosave_poll checks every 250 ms) */
{
    uint32_t end = fm1_ms + ms;
    while ((int32_t)(end - fm1_ms) > 0) {
        fm1_ms += 50u;
        autosave_poll();
    }
}

static void as_power_on(void)                            /* RAM lost: the defaults, then main.c's restore */
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
}

static void fill128(track_t *t, uint32_t seed)
{
    uint32_t i;
    t->p[P_SLEN] = 128;
    for (i = 0; i < NSTEP; i++) {
        step_clear(&t->step[i]);
        t->step[i].time = ST_NOTE; t->step[i].n = 1;
        t->step[i].note[0] = (uint8_t)(20u + (i * 7u + seed) % 90u);
        t->step[i].vel = (uint8_t)(40u + (i + seed) % 80u);
        if (i % 5u == 0u) t->step[i].hit = (uint8_t)(1u + i % 3u);
    }
}
static int same128(const track_t *a, const step_t *ref)
{
    return !memcmp(a->step, ref, NSTEP * sizeof(step_t));
}
int main(void)
{
    int bad = 0, ok;
    static step_t ref[NSTEP];
    int16_t v;
    uint32_t i;

    /* a project SAVE / LOAD through flash: 128 steps, LEN 128, the automation on steps from 64 up */
    reset();
    fill128(&trk[0], 3);
    memcpy(ref, trk[0].step, sizeof ref);
    ok = !motion_set_event(&trk[0], 5, P_REV, 60) && !motion_set_lock(&trk[0], 100, P_REV, 70) &&
         !motion_set_event(&trk[0], 127, P_DLY, 33) && !motion_set_lock(&trk[0], 64, P_REV, 12) && motion_count(&trk[0]) == 4u;
    project_save(0);
    ok &= !strcmp(ui.msg, "SAVED");
    bad += check("SAVE with 128 steps and records on steps 6, 65, 101, 128: saved", ok);
    memset(trk[0].step, 0, sizeof trk[0].step);
    for (i = 0; i < NSTEP; i++) step_clear(&trk[0].step[i]);
    trk[0].p[P_SLEN] = 16;
    motion_clear(&trk[0]);
    memset(proj_slot, 0, sizeof proj_slot);                          /* (RAM lost: the flash copy) */
    project_load(0);
    bad += check("LOAD (from flash): LEN 128 and every one of the 128 steps back", trk[0].p[P_SLEN] == 128 && same128(&trk[0], ref));
    bad += check("  the records on steps 6, 65, 101 and 128 back, with their values and kinds",
                 motion_count(&trk[0]) == 4u && motion_lock_get(&trk[0], 100, P_REV, &v) && v == 70 &&
                 motion_lock_get(&trk[0], 64, P_REV, &v) && v == 12 && !motion_lock_get(&trk[0], 5, P_REV, &v) &&
                 !motion_lock_get(&trk[0], 127, P_DLY, &v) && ss_has(motion_lock_steps(0), 100) && ss_has(motion_lock_steps(0), 64) && !ss_has(motion_lock_steps(0), 5) && !ss_has(motion_lock_steps(0), 127));

    /* a LEN of 100: below the byte the record keeps (127): exactly as it was */
    trk[0].p[P_SLEN] = 100;
    project_save(0);
    trk[0].p[P_SLEN] = 16;
    memset(proj_slot, 0, sizeof proj_slot);
    project_load(0);
    bad += check("LEN 100 (below the record's 127) comes back as 100; steps 65..100 as saved", trk[0].p[P_SLEN] == 100 && same128(&trk[0], ref));

    /* the autosave: the same, after a power cut */
    reset();
    fill128(&trk[1], 11);
    memcpy(ref, trk[1].step, sizeof ref);
    (void)motion_set_lock(&trk[1], 90, P_REV, 44);
    song.g[G_BPM] = 97;
    as_run(70000u);
    as_power_on();
    ok = autosave_boot(1) == 1;
    bad += check("autosave: power-on restores LEN 128, the 128 steps and a lock on step 91", ok && trk[1].p[P_SLEN] == 128 && same128(&trk[1], ref) &&
                 motion_lock_get(&trk[1], 90, P_REV, &v) && v == 44);

    /* sequences: switch away and back with 128 steps and automation above 64 */
    reset();
    fill128(&trk[0], 5);
    memcpy(ref, trk[0].step, sizeof ref);
    (void)motion_set_lock(&trk[0], 120, P_REV, 21);
    ok = pb_select(0, 3) == 0 && trk[0].p[P_SLEN] != 128 && !motion_count(&trk[0]);
    fill128(&trk[0], 9);
    trk[0].p[P_SLEN] = 128;
    ok &= pb_select(0, 0) == 0 && trk[0].p[P_SLEN] == 128 && same128(&trk[0], ref) && motion_lock_get(&trk[0], 120, P_REV, &v) && v == 21;
    bad += check("SEQUENCE 1 -> 4 -> 1: its 128 steps, LEN and the lock on step 121 as they were", ok);
    project_save(2);
    memset(proj_slot, 0, sizeof proj_slot);
    memset(trk[0].step, 0, sizeof trk[0].step);
    motion_clear(&trk[0]);
    project_load(2);
    ok = trk[0].p[P_SLEN] == 128 && same128(&trk[0], ref) && motion_lock_get(&trk[0], 120, P_REV, &v) && v == 21;
    ok &= pb_select(0, 3) == 0 && trk[0].p[P_SLEN] == 128;
    bad += check("  saved and loaded: the playing one is back, the other (128 steps too) is in the bank", ok);

    /* a SONG plays the steps from 65 up of a project's sequence (the record holds 64, the bank the rest) */
    reset();
    fill128(&trk[0], 21);
    memcpy(ref, trk[0].step, sizeof ref);
    (void)motion_set_lock(&trk[0], 110, P_REV, 55);
    project_save(1);
    memset(proj_slot + 0, 0, sizeof proj_slot[0]);
    chain_config.count = 1;
    chain_config.row[0] = chain_row_of(1, 1);
    ok = chain_prepare() == 0u;
    ok &= !memcmp(chain.source[1].step[0], ref, NSTEP * sizeof(step_t));
    {   uint32_t e, found = 0;
        for (e = 0; e < chain.source[1].motion.count; e++)
            found += MO_STEP(chain.source[1].motion.event[e].place) == 110u && chain.source[1].motion.event[e].value == 55;
        ok &= found == 1u;
    }
    bad += check("SONG: a project's sequence of 128 steps and its lock on step 111 are the section's", ok);

    /* the header's sequence memory (pb_use_pct): 0 when nothing is kept, more as the sequences fill it, never above 100; LEN shows no unit */
    reset();
    {
        uint32_t p0, p1, p2;
        char val[12];
        const char *unit = "x";
        for (i = 0; i < NTRK * NSTEP; i++) step_clear(&trk[i / NSTEP].step[i % NSTEP]);
        pb_clear_all();
        pb_sync_all();
        p0 = pb_use_pct();
        fill128(&trk[0], 2);
        pb_sync_all();
        p1 = pb_use_pct();
        fill128(&trk[1], 4); fill128(&trk[2], 6); fill128(&trk[3], 8);
        for (i = 1; i < 6; i++) {                        /* five more full sequences in the arena of track 1 */
            pb_select(1, i);
            fill128(&trk[1], 10 + i);
        }
        pb_sync_all();
        p2 = pb_use_pct();
        param_format(&TP[P_SLEN], 128, val, &unit);
        bad += check("SEQUENCE MEMORY: 0 % empty, more with a full 128-step sequence, more with its neighbours, 100 at most; LEN 128 has no unit",
                     p0 == 0u && p1 > p0 && p2 >= p1 && p2 <= 100u && !strcmp(val, "128") && unit[0] == 0);
    }

    /* the sequencer: from step 1 to step 128 and round */
    reset();
    fill128(&trk[0], 1);
    trk[0].p[P_SDIV] = 2;
    song.sel = 0;
    seq_start();
    seq_tick(&trk[0], CTL);
    {
        uint32_t seen_last = 0, wrapped = 0, prev = trk[0].seq_idx, n;
        for (n = 0; n < 4000000u && !wrapped; n++) {
            seq_tick(&trk[0], 64);
            if (trk[0].seq_idx == 127u) seen_last = 1;
            if (seen_last && trk[0].seq_idx < prev) wrapped = 1;
            prev = trk[0].seq_idx;
        }
        bad += check("PLAY: LEN 128 plays step 128 and starts again at step 1", seen_last && wrapped && trk[0].seq_idx < 4u);
    }
    seq_stop();

    if (bad) { printf("STEPS128 TEST FAILED\n"); return 1; }
    printf("128 step sequence tests passed\n");
    return 0;
}
