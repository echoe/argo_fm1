/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* The sequence banks in flash (patbank.c): store s (0..3 the projects A..D, PB_LIVE the music as it is now) has room for six
 * objects OBJ_PB0 + 6 s + o, each an A/B pair of storage.c, and uses five. The sparse format (version 4, since 1.8.7):
 *   object t < 4   the eight sequences of track t. Payload: 8 bytes {'P', 'B', 4, t, GEN's DENS, CHORD, EVOLVE, ALGO of the
 *                  track}, then per sequence a list: its bitmap size B (8 for 64 steps), the bitmap (bit i of the bytes: step i
 *                  has something in it), then those steps in order, each a mask byte (bit k < 7: byte k of the step's nine
 *                  packed bytes, patbank.c pb_pack_step, differs from the empty step's; bit 7: bytes 7 and 8 do) and the bytes
 *                  that differ. An empty sequence is 9 bytes.
 *   object 4       the meta object: 8 bytes {'P', 'B', 4, 4, the four chain masks}, the sequence each track plays (4), the 32
 *                  LENs, per sequence its automation record count and ON (2 bytes), then the records of the sequences with
 *                  some (3 bytes each, in order; the playing sequence's are in the project record, not here: at most
 *                  PB_META_RECS, more is refused: pb_meta_ok), then the chord packs (chord.c; absent in a store converted
 *                  from format 1 / 2).
 *   object 5       free (the OPX engine's). It was the old meta object.
 * The old format (versions 1..3, up to 1.8.6): objects 0..4 six dense sequences each (576 bytes, q = track * 8 + slot, 3464
 * bytes), object 5 the last two sequences and the meta data (the same as object 4 now, after the two sequences).
 * pb_store_convert turns a store of the old format into the new one, in place: for each track in turn it reads the track's
 * sequences from the old objects (an old object j holds sequences of tracks <= j only, so writing the new object j never
 * takes what a later track still needs), writes the track's object, and last the meta object over old object 4. A power cut
 * leaves some tracks converted and the old meta object (5) intact: the next call goes on from the first track not done. A
 * store is "new" when its meta object is in the new format; pb_convert_all (autosave_boot) converts every store at power-on,
 * pb_store_save of an old store writes it whole (the music being saved is the whole store). The GEN knobs of the tracks not
 * yet converted are lost if the power is cut during a conversion (the defaults come back); nothing else is. */
#if FELUCCA_PATBANK && FELUCCA_FLASH
#define PB_META_FIX (PB_HDR + NTRK + PB_NSEQ + 2u * PB_NSEQ)
#define PB_OLD_META_FIX (PB_HDR + 2u * PB_OLD_PAT_B + NTRK + PB_NSEQ + 2u * PB_NSEQ)
#define PB_CPK_B ((1u + CPK_USER) * (uint32_t)sizeof(cpack_t))   /* the chord packs (chord.c), after the records */
#define PB_REC_B 3u                              /* an automation record on flash: place (track << 6 | step), id, value (as the project record's) */
#define PB_UX_HDR 6u                             /* 1.9, the sections after the chord packs: 'U', 'X', 1, 0, the count (2 bytes), then per record 4 bytes: */
#define PB_UX_REC 4u                             /* the sequence q (track * 8 + slot), the step (NSTEP_REC..127), id, value: the records of the steps from 64 up */
/* The object's RAM is the project staging RAM (project.c proj_wire_u): a save, a load and the autosave already take turns with
 * it, and a backup noticing proj_wire_gen knows it was reused. It is not kept a 3.8 KB buffer of its own: the pool is full */
#define pb_wire (proj_wire_u.raw)
_Static_assert(sizeof proj_wire_u.raw >= PB_OBJ_MAX, "the sequence bank objects fit the project staging RAM");
_Static_assert(GS_N <= 4u && NTRK <= 4u, "the four bytes of an object's header");
_Static_assert(PB_BM <= 16u, "a bitmap is at most 16 bytes");
#define PB_WIRE_USE() (proj_wire_gen++)

/* a record on flash (steps below NSTEP_REC: the 3 bytes the project record has) <-> as it is in RAM */
static void pb_rec_put(uint8_t *d, const motion_event_t *e)
{
    d[0] = (uint8_t)(MO_TRK(e->place) << 6 | MO_STEP(e->place));
    d[1] = e->param;
    d[2] = (uint8_t)e->value;
}
static void pb_rec_get(motion_event_t *e, const uint8_t *d)
{
    e->place = MO_PLACE(d[0] >> 6, d[0] & 63u);
    e->param = d[1];
    e->value = (int8_t)d[2];
}
static uint32_t pb_lo_n(uint32_t q)              /* sequence q's records on steps below NSTEP_REC (not playing: its mirror) */
{
    uint32_t i, n = 0;
    for (i = 0; i < pb_mo[q].count && i < PB_MO_MAX; i++)
        n += pb_mo[q].ev[i].step < NSTEP_REC;
    return n;
}
static uint32_t pb_hi_n(void)                    /* the records from step NSTEP_REC up: of the sequences not playing, and of the playing ones (the live ones) */
{
    uint32_t q, i, n = 0;
    for (q = 0; q < PB_NSEQ; q++)
        if (q % PB_N != pb_cur[q / PB_N])
            for (i = 0; i < pb_mo[q].count && i < PB_MO_MAX; i++)
                n += pb_mo[q].ev[i].step >= NSTEP_REC;
    for (i = 0; i < motion.count; i++)
        n += MO_STEP(motion.event[i].place) >= NSTEP_REC;
    return n;
}
static uint32_t pb_meta_bytes(void)              /* the meta object as pb_build writes it */
{
    uint32_t q, lo = 0, hi = pb_hi_n();
    for (q = 0; q < PB_NSEQ; q++)
        if (q % PB_N != pb_cur[q / PB_N])
            lo += pb_lo_n(q);
    return PB_META_FIX + PB_REC_B * lo + PB_CPK_B + (hi ? PB_UX_HDR + PB_UX_REC * hi : 0u);
}
static int pb_meta_ok(void) { return pb_meta_bytes() <= PB_OBJ_MAX; }

/* ---- objects ---- */
static uint32_t pb_track_size(uint32_t t)
{
    uint32_t k, n = PB_HDR;
    for (k = 0; k < PB_N; k++)
        n += pb_list_size(t, k);
    return n;
}

/* 0..100: how full what holds the sequences is: the arena (the sequences not playing: a switch that finds it full is refused) or a
 * track's flash object (its eight sequences: a SAVE that finds it full is refused), the fuller of the two. The header shows it
 * (ui_draw.c); it is worked out again when the sequences changed (pb_gen) or the arena did */
static uint32_t pb_use_pct(void)
{
    static uint32_t gen = ~0u, au = ~0u, worst;
    uint32_t t, a = pb_aused();
    if (gen != pb_gen || au != a) {
        uint32_t empty = PB_HDR + PB_N * (1u + PB_BM);
        gen = pb_gen;
        au = a;
        worst = (a * 100u + PB_ARENA_B - 1u) / PB_ARENA_B;
        for (t = 0; t < NTRK; t++) {
            uint32_t n = pb_track_size(t), p;
            p = n > empty ? ((n - empty) * 100u + (PB_OBJ_MAX - empty) - 1u) / (PB_OBJ_MAX - empty) : 0u;
            if (p > worst)
                worst = p;
        }
        if (worst > 100u)
            worst = 100u;
    }
    return worst;
}

static void pb_head(uint32_t kind)
{
    memset(pb_wire, 0, PB_HDR);
    pb_wire[0] = 'P'; pb_wire[1] = 'B'; pb_wire[2] = (uint8_t)PB_FMT; pb_wire[3] = (uint8_t)kind;
}
static int pb_hdr_ok(uint32_t kind)         /* pb_wire holds an object of the new format, of this kind */
{
    return pb_wire[0] == 'P' && pb_wire[1] == 'B' && pb_wire[2] == PB_FMT && pb_wire[3] == kind;
}

/* object o -> pb_wire; its length */
static uint32_t pb_build(uint32_t o)
{
    PB_WIRE_USE();
    uint32_t pos = PB_HDR, q, k;
    pb_head(o);
    if (o < NTRK) {
        for (k = 0; k < GS_N; k++)                      /* GEN's knobs of the track */
            pb_wire[4u + k] = gen_set[o][k];
        for (k = 0; k < PB_N; k++)
            pos += pb_list_put(pb_wire + pos, o, k);
        return pos;
    }
    for (q = 0; q < NTRK; q++)                          /* CHAIN: per track the mask of the chained sequences */
        pb_wire[4u + q] = pb_cmask[q];
    memcpy(pb_wire + pos, pb_cur, NTRK); pos += NTRK;
    memcpy(pb_wire + pos, pb_len, PB_NSEQ); pos += PB_NSEQ;
    for (q = 0; q < PB_NSEQ; q++) {
        int on = q % PB_N != pb_cur[q / PB_N];
        pb_wire[pos++] = on ? (uint8_t)pb_lo_n(q) : 0;
        pb_wire[pos++] = on ? pb_mo[q].on : 0;
    }
    for (q = 0; q < PB_NSEQ; q++)
        if (q % PB_N != pb_cur[q / PB_N])
            for (k = 0; k < pb_mo[q].count && k < PB_MO_MAX; k++)
                if (pb_mo[q].ev[k].step < NSTEP_REC) {
                    motion_event_t ev;
                    pb_ev_to(&ev, q / PB_N, &pb_mo[q].ev[k]);
                    pb_rec_put(pb_wire + pos, &ev);
                    pos += PB_REC_B;
                }
    memcpy(pb_wire + pos, &cpk, sizeof cpk); pos += (uint32_t)sizeof cpk;           /* the chord packs: what the black keys do, the user's */
    for (q = 0; q < CPK_USER; q++) {
        memcpy(pb_wire + pos, &cpk_user[q], sizeof(cpack_t));
        pos += (uint32_t)sizeof(cpack_t);
    }
    {   /* 1.9: the records of the steps from NSTEP_REC up (a project record and a firmware before hold none of them; they ignore this) */
        uint32_t hi = pb_hi_n(), cnt = 0, i;
        if (hi) {
            pb_wire[pos] = 'U'; pb_wire[pos + 1u] = 'X'; pb_wire[pos + 2u] = 1; pb_wire[pos + 3u] = 0;
            pb_wire[pos + 4u] = (uint8_t)hi; pb_wire[pos + 5u] = (uint8_t)(hi >> 8);
            pos += PB_UX_HDR;
            for (q = 0; q < PB_NSEQ; q++) {
                int playing = q % PB_N == pb_cur[q / PB_N];
                if (!playing)
                    for (i = 0; i < pb_mo[q].count && i < PB_MO_MAX; i++) {
                        const pb_ev_t *e = &pb_mo[q].ev[i];
                        if (e->step >= NSTEP_REC && cnt < hi) {
                            pb_wire[pos++] = (uint8_t)q; pb_wire[pos++] = e->step;
                            pb_wire[pos++] = e->param; pb_wire[pos++] = (uint8_t)e->value;
                            cnt++;
                        }
                    }
                else
                    for (i = 0; i < motion.count; i++) {
                        const motion_event_t *e = &motion.event[i];
                        if (MO_TRK(e->place) == q / PB_N && MO_STEP(e->place) >= NSTEP_REC && cnt < hi) {
                            pb_wire[pos++] = (uint8_t)q; pb_wire[pos++] = (uint8_t)MO_STEP(e->place);
                            pb_wire[pos++] = e->param; pb_wire[pos++] = (uint8_t)e->value;
                            cnt++;
                        }
                    }
            }
        }
    }
    return pos;
}

/* store s's meta object, new format -> pb_wire: its length, or -1 (none, damaged, or of the old format) */
static int pb_new_meta(uint32_t s)
{
    int n = st_load(OBJ_PB0 + s * PB_NOBJ + PB_META_O, pb_wire, sizeof pb_wire);
    if (n < (int)PB_META_FIX || !pb_hdr_ok(PB_META_O))
        return -1;
    return n;
}

/* ---- the old format -> the new, in place ---- */
static const uint8_t PB_GEN_DEF[GS_N] = {64, 0, 24, 0};   /* (seq.c gen_set's first values: what an unreadable one gets) */

/* the old format's sequence q (0..31) of store s -> pat. 1 found; 0 none (pat is the empty one): no such object, it is no
 * old one (a new one over it, as the conversion goes) */
static int pb_old_seq(uint32_t s, uint32_t q, uint8_t *pat)
{
    uint32_t len, o = q / PB_PER, off;
    const uint8_t *v = st_view(OBJ_PB0 + s * PB_NOBJ + o, &len);
    pb_pat_empty(pat);
    if (!v || v[0] != 'P' || v[1] != 'B' || v[3] != o)
        return 0;
    if (o < PB_OLD_META_O) {
        if ((v[2] != 1 && v[2] != 2) || len != PB_HDR + PB_PER * PB_OLD_PAT_B)
            return 0;
        off = PB_HDR + (q % PB_PER) * PB_OLD_PAT_B;
    } else {
        if (v[2] < 1 || v[2] > 3 || len < PB_OLD_META_FIX)
            return 0;
        off = PB_HDR + (q - PB_PER * PB_OLD_META_O) * PB_OLD_PAT_B;
    }
    memcpy(pat, v + off, PB_OLD_PAT_B);                 /* (the first NSTEP_REC steps: the rest stays empty) */
    return 1;
}

/* store s into the new format. 1: it is (it was, or now is); 0: there is nothing to convert (no old meta object: no store);
 * -1: a flash error (called again, it goes on) */
static int pb_store_convert(uint32_t s)
{
    uint32_t base = OBJ_PB0 + s * PB_NOBJ, t, k, len, n;
    uint8_t gen[NTRK][GS_N], pat[PB_PAT_B];
    const uint8_t *v;
    PB_WIRE_USE();
    if (pb_new_meta(s) >= 0)
        return 1;
    v = st_view(base + PB_OLD_META_O, &len);
    if (!v || len < PB_OLD_META_FIX || v[0] != 'P' || v[1] != 'B' || v[2] < 1 || v[2] > 3 || v[3] != PB_OLD_META_O)
        return 0;
    for (k = 0; k < GS_N; k++) {                        /* GEN's knobs: the old objects 0..3's headers, before any is overwritten */
        v = st_view(base + k, &len);
        for (t = 0; t < NTRK; t++)
            gen[t][k] = v && len >= PB_HDR && v[0] == 'P' && v[1] == 'B' && v[2] == 2 && v[3] == k && v[4u + t] < 128u ?
                        v[4u + t] : PB_GEN_DEF[k];
    }
    for (t = 0; t < NTRK; t++) {
        uint32_t pos = PB_HDR;
        PB_WIRE_USE();
        n = (uint32_t)st_load(base + t, pb_wire, sizeof pb_wire);
        if ((int)n >= (int)PB_HDR && pb_hdr_ok(t))      /* (done before a power cut) */
            continue;
        pb_head(t);
        for (k = 0; k < GS_N; k++)
            pb_wire[4u + k] = gen[t][k];
        for (k = 0; k < PB_N; k++) {
            uint32_t rest = (PB_N - 1u - k) * (1u + PB_BM);   /* (room for the empty ones after it) */
            (void)pb_old_seq(s, t * PB_N + k, pat);
            if (pos + pb_seq_size(pat) + rest > PB_OBJ_MAX)   /* it does not fit: the sequence is not kept */
                pb_pat_empty(pat);
            pos += pb_seq_put(pb_wire + pos, pat);
        }
        if (st_save(base + t, pb_wire, pos))
            return -1;
    }
    PB_WIRE_USE();
    n = (uint32_t)st_load(base + PB_OLD_META_O, pb_wire, sizeof pb_wire);   /* the meta object: the old one without its two sequences */
    if ((int)n < (int)PB_OLD_META_FIX)
        return 0;
    for (k = PB_HDR; k < n - 2u * PB_OLD_PAT_B; k++)        /* (the firmware's libc.c has no memmove: a forward copy, the destination is below the source) */
        pb_wire[k] = pb_wire[k + 2u * PB_OLD_PAT_B];
    n -= 2u * PB_OLD_PAT_B;
    if (pb_wire[2] < 2)                                 /* (version 1 had no chain masks) */
        memset(pb_wire + 4, 0, 4);
    pb_wire[2] = (uint8_t)PB_FMT;
    pb_wire[3] = (uint8_t)PB_META_O;
    return st_save(base + PB_META_O, pb_wire, n) ? -1 : 1;
}
static void pb_convert_all(void)                        /* power-on (project.c autosave_boot): every store, so that the sixth objects are free */
{
    uint32_t s;
    for (s = 0; s < PB_NSTORE; s++)
        (void)pb_store_convert(s);
}
/* store s's sixth object holds nothing of the sequence banks, so that the OPX engine may write it (opx_store.c, a backup
 * restore): the store is in the sparse format, or has no old meta object to lose. 0 when the old one is still there (the
 * conversion at power-on failed: nothing is written over it; the next power-on tries again). Looks through st_view: the
 * callers' buffers (pb_wire is proj_wire: a backup's chunk is in it) are not touched */
static int pb_sixth_free(uint32_t s)
{
    static uint8_t ok;
    uint32_t len;
    const uint8_t *v;
    if ((ok >> s) & 1u)
        return 1;
    v = st_view(OBJ_PB0 + s * PB_NOBJ + PB_META_O, &len);
    if (v && len >= PB_META_FIX && v[0] == 'P' && v[1] == 'B' && v[2] == PB_FMT && v[3] == PB_META_O) {
        ok |= (uint8_t)(1u << s);
        return 1;
    }
    v = st_view(OBJ_PB0 + s * PB_NOBJ + PB_OLD_META_O, &len);
    if (v && len >= PB_OLD_META_FIX && v[0] == 'P' && v[1] == 'B' && v[2] >= 1 && v[2] <= 3 && v[3] == PB_OLD_META_O)
        return 0;
    ok |= (uint8_t)(1u << s);
    return 1;
}
/* store s's meta object -> pb_wire (an old store is converted first): its length, or -1 */
static int pb_meta_for(uint32_t s)
{
    int n = pb_new_meta(s);
    if (n < 0 && pb_store_convert(s) == 1)
        n = pb_new_meta(s);
    return n;
}

/* the objects of `mask` of store s -> flash; 0 ok, 2 a flash error, 3 too much automation, 4 a track's sequences do not
 * fit its object. Syncs the playing sequences first. A store that is not in the new format is written whole, in order (the
 * meta object last makes it the new format's) */
static int pb_store_save(uint32_t s, uint32_t mask)
{
    uint32_t o, t;
    pb_need();
    pb_sync_all();
    if (!pb_meta_ok())
        return 3;
    if (pb_new_meta(s) < 0)
        mask = PB_ALLOBJ;
    for (t = 0; t < NTRK; t++)
        if (((mask >> t) & 1u) && pb_track_size(t) > PB_OBJ_MAX)
            return 4;
    for (o = 0; o < PB_NUSED; o++)
        if ((mask >> o) & 1u) {
            uint32_t n = pb_build(o);
            if (st_save(OBJ_PB0 + s * PB_NOBJ + o, pb_wire, n))
                return 2;
        }
    return 0;
}

/* the track object t of store s, in pb_wire, holds eight lists that parse: 1 */
static int pb_track_ok(uint32_t n)
{
    uint32_t k, p = PB_HDR, sz;
    uint8_t sc[PB_PAT_B];
    for (k = 0; k < PB_N; k++) {
        if (p >= n || (sz = pb_seq_get(sc, pb_wire + p, n - p)) == 0u)
            return 0;
        p += sz;
    }
    return 1;
}

/* the upper records (PB_UX_*) of the meta object in pb_wire (n bytes, its records end at `pos`): the section's offset, 0 none */
static uint32_t pb_ux_pos(uint32_t n)
{
    uint32_t q, tot = 0, p, cnt;
    for (q = 0; q < PB_NSEQ; q++)
        tot += pb_wire[PB_HDR + NTRK + PB_NSEQ + 2u * q];
    p = PB_META_FIX + PB_REC_B * tot + PB_CPK_B;
    if (p + PB_UX_HDR > n || pb_wire[p] != 'U' || pb_wire[p + 1u] != 'X' || pb_wire[p + 2u] != 1u)
        return 0;
    cnt = pb_wire[p + 4u] | (uint32_t)pb_wire[p + 5u] << 8;
    return p + PB_UX_HDR + PB_UX_REC * cnt <= n ? p : 0u;
}
static uint32_t pb_ux_count(uint32_t p) { return pb_wire[p + 4u] | (uint32_t)pb_wire[p + 5u] << 8; }

/* a record of a playing sequence from the bank (step NSTEP_REC up) into the live automation: not twice, not past MOTION_MAX */
static void pb_motion_add(const motion_event_t *e)
{
    uint32_t i, f;
    if (!motion_param(MOTION_ID(e)) || e->value < -64 || e->value > 127)
        return;
    f = motion_guard();
    for (i = 0; i < motion.count; i++)
        if (motion.event[i].place == e->place && MOTION_ID(&motion.event[i]) == MOTION_ID(e))
            break;
    if (i == motion.count && motion.count < MOTION_MAX) {
        motion.event[motion.count] = *e;
        RING_PUBLISH();
        motion.count++;
    }
    motion_unguard(f);
}

/* store s -> the mirror, after the music (steps, LEN, automation: the playing sequences) was loaded; 0: no such store
 * (the mirror holds just the loaded music, pb_reset_to_live) */
static int pb_store_load(uint32_t s)
{
    PB_WIRE_USE();
    uint32_t q, t, pos, n, k;
    uint8_t cur[NTRK], len[PB_NSEQ], cnt[PB_NSEQ], on[PB_NSEQ];
    int nn;
    pb_reset_to_live();
    nn = pb_meta_for(s);
    if (nn < 0)
        return 0;
    n = (uint32_t)nn;
    pos = PB_HDR;
    memcpy(cur, pb_wire + pos, NTRK); pos += NTRK;
    memcpy(len, pb_wire + pos, PB_NSEQ); pos += PB_NSEQ;
    for (q = 0; q < PB_NSEQ; q++) {
        cnt[q] = pb_wire[pos++];
        on[q] = pb_wire[pos++];
    }
    for (t = 0; t < NTRK; t++)
        if (cur[t] >= PB_N)
            return 0;
    {   /* the records, checked against the length before any of it is kept */
        uint32_t tot = 0;
        for (q = 0; q < PB_NSEQ; q++) {
            if (cnt[q] > PB_MO_MAX) return 0;
            tot += cnt[q];
        }
        if (pos + tot * PB_REC_B > n) return 0;
    }
    for (t = 0; t < NTRK; t++)                          /* CHAIN: the masks */
        pb_cmask[t] = pb_wire[4u + t];
    for (q = 0; q < PB_NSEQ; q++) {
        pb_mo[q].count = 0;
        if (!cnt[q])
            continue;
        pb_mo[q].on = on[q] != 0u;
        for (k = 0; k < cnt[q]; k++, pos += PB_REC_B)
            { motion_event_t ev; pb_rec_get(&ev, pb_wire + pos); pb_ev_from(&pb_mo[q].ev[k], &ev); }
        pb_mo[q].count = cnt[q];
    }
    {   /* 1.9: the records from step NSTEP_REC up: of the sequences not playing into their mirror, of the playing ones into the live automation */
        uint32_t ux = pb_ux_pos(n), j;
        if (ux)
            for (j = 0; j < pb_ux_count(ux); j++) {
                const uint8_t *r = pb_wire + ux + PB_UX_HDR + PB_UX_REC * j;
                uint32_t qq = r[0];
                motion_event_t e;
                if (qq >= PB_NSEQ || r[1] < NSTEP_REC || r[1] >= NSTEP)
                    continue;
                e.place = MO_PLACE(qq / PB_N, r[1]); e.param = r[2]; e.value = (int8_t)r[3];
                if (qq % PB_N == cur[qq / PB_N])
                    pb_motion_add(&e);
                else if (pb_mo[qq].count < PB_MO_MAX && motion_param(MOTION_ID(&e))) {
                    pb_ev_from(&pb_mo[qq].ev[pb_mo[qq].count++], &e);
                    pb_mo[qq].on = on[qq] != 0u;
                }
            }
    }
    if (pos + PB_CPK_B <= n) {                          /* the chord packs (a store converted from format 1 / 2: the built in ones pb_reset_to_live set) */
        cpack_t pk[1u + CPK_USER];
        uint32_t ok = 1;
        memcpy(pk, pb_wire + pos, sizeof pk);
        for (q = 0; q < 1u + CPK_USER; q++)
            ok &= (uint32_t)cpk_valid(&pk[q]);
        if (ok) {
            cpk = pk[0];
            for (q = 0; q < CPK_USER; q++)
                cpk_user[q] = pk[1u + q];
        }
    }
    for (q = 0; q < PB_NSEQ; q++)
        pb_len[q] = len[q] >= (uint32_t)TP[P_SLEN].min && len[q] <= (uint32_t)TP[P_SLEN].max ? len[q] : (uint8_t)TP[P_SLEN].def;
    memset(pb_asz, 0, sizeof pb_asz);                   /* (the arena: every sequence empty, the objects fill it) */
    for (t = 0; t < NTRK; t++) {                        /* the tracks' sequences; one that does not parse stays empty */
        n = (uint32_t)st_load(OBJ_PB0 + s * PB_NOBJ + t, pb_wire, sizeof pb_wire);
        if ((int)n >= (int)PB_HDR && pb_hdr_ok(t) && pb_track_ok(n)) {
            pos = PB_HDR;
            for (k = 0; k < PB_N; k++) {
                uint8_t pat[PB_PAT_B];
                pos += pb_seq_get(pat, pb_wire + pos, n - pos);
                if (k == cur[t]) {                      /* 1.9: the playing one: its steps from NSTEP_REC up are the bank's (the project record holds the first NSTEP_REC) */
                    uint32_t i, f = motion_guard();
                    for (i = NSTEP_REC; i < NSTEP; i++)
                        if (!pb_unpack_step(&trk[t].step[i], pat + i * PB_STEP_B))
                            step_clear(&trk[t].step[i]);
                    motion_unguard(f);
                } else
                    (void)pb_mput(t * PB_N + k, pat);   /* (the arena full: the sequence is not kept) */
            }
            for (k = 0; k < GS_N; k++)                  /* GEN's knobs */
                gen_set[t][k] = pb_wire[4u + k] < 128u ? pb_wire[4u + k] : gen_set[t][k];
        }
    }
    for (t = 0; t < NTRK; t++) {            /* the sequence a track plays is the loaded music's (the record) */
        pb_cur[t] = cur[t];
        pb_mo[t * PB_N + cur[t]].count = 0;
        {   /* 1.9: its LEN of 128 (the record holds 127) */
            uint32_t qq = t * PB_N + cur[t], f = motion_guard();
            if (trk[t].p[P_SLEN] >= (int16_t)LEN_REC && pb_len[qq] > LEN_REC)
                trk[t].p[P_SLEN] = (int16_t)clamp((int32_t)pb_len[qq], TP[P_SLEN].min, TP[P_SLEN].max);
            motion_unguard(f);
        }
        pb_rehash(t);
        pb_sync(t);
    }
    pb_dirty = PB_ALLOBJ;
    pb_gen++;
    return 1;
}


/* A song (song_chain.c, project.c chain_prepare) plays other projects' sequences: store s's sequence the project was saved on
 * for each track (its record plays that one); 0 when there is no such store (the record's is "sequence 1") */
static int pb_store_saved(uint32_t s, uint8_t *cur)
{
    PB_WIRE_USE();
    uint32_t t;
    memset(cur, 0, NTRK);
    if (pb_meta_for(s) < 0)
        return 0;
    for (t = 0; t < NTRK; t++)
        cur[t] = pb_wire[PB_HDR + t] < PB_N ? pb_wire[PB_HDR + t] : 0u;
    return 1;
}
/* store s's sequence q of track k, for a song: its steps (st, NSTEP), LEN and automation (mo: the track's records). 0 done;
 * 1 no such store or object (the sequence is empty, LEN the default) */
static int pb_store_fetch(uint32_t s, uint32_t k, uint32_t q, step_t *st, uint8_t *len, pb_mo_t *mo)
{
    PB_WIRE_USE();
    uint32_t n, qi = k * PB_N + q, pos, i, skip = 0, cnt, p, sz;
    uint8_t pat[PB_PAT_B];
    int nn;
    for (i = 0; i < NSTEP; i++)
        step_clear(&st[i]);
    *len = (uint8_t)TP[P_SLEN].def;
    memset(mo, 0, sizeof *mo);
    nn = pb_meta_for(s);
    if (nn < 0)
        return 1;
    n = (uint32_t)nn;
    pos = PB_HDR + NTRK;
    if (pb_wire[pos + qi] >= (uint32_t)TP[P_SLEN].min && pb_wire[pos + qi] <= (uint32_t)TP[P_SLEN].max)
        *len = pb_wire[pos + qi];
    pos += PB_NSEQ;                                      /* the (count, ON) pairs, then the records of those with some */
    for (i = 0; i < qi; i++)
        skip += pb_wire[pos + 2u * i];
    cnt = pb_wire[pos + 2u * qi];
    if (cnt && cnt <= PB_MO_MAX && pos + 2u * PB_NSEQ + (skip + cnt) * PB_REC_B <= n) {
        for (i = 0; i < cnt; i++)
            { motion_event_t ev; pb_rec_get(&ev, pb_wire + pos + 2u * PB_NSEQ + (skip + i) * PB_REC_B); pb_ev_from(&mo->ev[i], &ev); }
        mo->count = (uint8_t)cnt;
        mo->on = pb_wire[pos + 2u * qi + 1u] != 0u;
    }
    {   /* 1.9: and the records from step NSTEP_REC up (for the sequence a project was saved on, only these: its others are in the project record) */
        uint32_t ux = pb_ux_pos(n), j;
        if (ux)
            for (j = 0; j < pb_ux_count(ux) && mo->count < PB_MO_MAX; j++) {
                const uint8_t *r = pb_wire + ux + PB_UX_HDR + PB_UX_REC * j;
                if (r[0] != qi || r[1] < NSTEP_REC || r[1] >= NSTEP)
                    continue;
                mo->ev[mo->count].step = r[1];
                mo->ev[mo->count].param = r[2];
                mo->ev[mo->count].value = (int8_t)r[3];
                mo->count++;
                mo->on = mo->on || pb_wire[pos + 2u * qi + 1u] != 0u;
            }
    }
    n = (uint32_t)st_load(OBJ_PB0 + s * PB_NOBJ + k, pb_wire, sizeof pb_wire);
    if ((int)n < (int)PB_HDR || !pb_hdr_ok(k) || !pb_track_ok(n))
        return 0;
    p = PB_HDR;
    for (i = 0; i < q; i++)                              /* (the lists before it) */
        p += pb_seq_get(pat, pb_wire + p, n - p);
    sz = pb_seq_get(pat, pb_wire + p, n - p);
    if (sz)
        (void)pb_unpack_pat(st, pat);
    return 0;
}

/* the autosave's companion (project.c autosave_poll): the live store, the objects that changed; 0 ok */
static int pb_flush_live(void)
{
    uint32_t m;
    pb_sync_all();
    m = pb_dirty;
    if (!m)
        return 0;
    if (pb_store_save(PB_LIVE, m))
        return 1;
    pb_dirty &= (uint8_t)~m;
    return 0;
}
#endif
