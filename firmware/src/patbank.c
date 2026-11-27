/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Sequences: eight per track (FELUCCA_PATBANK). A track plays one of them at a time: trk[].step is the one playing, the
 * other seven live in a RAM mirror (pb_pat, packed as a project packs steps: 9 bytes). Each sequence has its own steps, its
 * own LEN (P_SLEN) and its own automation (the track's motion records, at most PB_MO_MAX = 64 of them while it is not the
 * one playing). DIV, SWING, GATE, QUANTIZE and the sound stay the track's.
 * Switching (pb_select): the playing sequence is packed into the mirror, the chosen one is put into the track, at once,
 * in the middle of a pass (the playhead keeps its step). Main loop only.
 * Persistence (patbank_store.c): the project record keeps the sequence that plays; the others are in flash, five stores
 * (project A..D, the live music), each with six objects' room (the addresses are storage.c's) of which five are used since
 * the sparse format (1.8.7): object t < 4 holds the eight sequences of track t as lists (a bitmap of the steps that have
 * anything in them, then those steps), object 4 the rest (the sequence each track plays, the LENs, the automation of the
 * sequences not playing, the chord packs). The sixth object of each store is free: what was the old meta object (format 1..3:
 * object o < 5 six dense sequences q = track * 8 + slot, object 5 the last two and the rest) becomes the OPX engine's. A store
 * of the old format is converted in place, object by object, by pb_store_convert (patbank_store.c). */
#if FELUCCA_PATBANK
#define PB_N 8u                          /* sequences per track */
#define PB_NSEQ (NTRK * PB_N)
#define PB_STEP_B 9u                     /* a step packed: project.c proj_pack's nine bytes */
#define PB_PAT_B (NSTEP * PB_STEP_B)
#define PB_OLD_PAT_B (NSTEP_REC * PB_STEP_B)   /* a sequence of the old formats (1..3) and a project record: 64 steps */
#define PB_MO_MAX 64u                    /* automation records of a sequence that is not playing */
#define PB_PER 6u                        /* sequences an object of the OLD format (1..3) held */
#define PB_NOBJ 6u                       /* the objects a store has room for (OBJ_PB0 + 6 s + o) */
#define PB_NUSED 5u                      /* the ones in use: the tracks' 0..3 and the meta object 4 */
#define PB_ALLOBJ 0x1Fu                  /* (pb_dirty, the mask of a whole store) */
#define PB_NSTORE 5u
#define PB_LIVE 4u                       /* the live music's store */
#define PB_HDR 8u
#define PB_META_O 4u                     /* (the old format's was 5: PB_OLD_META_O) */
#define PB_OLD_META_O 5u
#define PB_FMT 4u                        /* the sparse format's version byte */
#define PB_BM ((NSTEP + 7u) / 8u)        /* a sequence's bitmap: bit i set, step i has something in it */
#define PB_OBJ_MAX ST_PAYLOAD_MAX
/* an automation record of a sequence not playing (the track is the sequence's: only the step, 0..127, is kept: 3 bytes, as on flash) */
typedef struct { uint8_t step, param; int8_t value; } pb_ev_t;
typedef struct { uint8_t count, on; pb_ev_t ev[PB_MO_MAX]; } pb_mo_t;
static void pb_ev_from(pb_ev_t *d, const motion_event_t *e) { d->step = (uint8_t)MO_STEP(e->place); d->param = e->param; d->value = e->value; }
static void pb_ev_to(motion_event_t *e, uint32_t t, const pb_ev_t *s) { e->place = MO_PLACE(t, s->step); e->param = s->param; e->value = s->value; }

static uint8_t pb_cur[NTRK];             /* the sequence each track plays, 0..7 */
static uint8_t pb_len[PB_NSEQ];          /* LEN of each (the playing one's is the track's P_SLEN) */
static pb_mo_t pb_mo[PB_NSEQ] __attribute__((section(".pool")));   /* (the playing sequence's: motion) */
static uint8_t pb_next[NTRK];            /* a switch waiting for the end of the sequence playing: the slot + 1, 0 none (SEQ SWITCH END) */
static uint8_t pb_last[NTRK];            /* the step each track was on when pb_due last looked */
static uint8_t pb_fired[NTRK];           /* a switch happened on this pass's last step: not again until the pass is under way */
static uint8_t pb_cmask[NTRK];           /* CHAIN: bit s set, sequence s of the track is one the user chained (patbank.c pb_chain_next) */
static uint8_t pb_ready;
static uint8_t pb_dirty;                 /* bit o: the live store's object o (track o's sequences, 4 the meta) is older than the mirror */
static uint32_t pb_gen;                  /* + 1 per change (autosave_sig) */

/* ---- steps <-> bytes: the bytes of project.c proj_pack / proj_unpack ---- */
static void pb_pack_step(uint8_t *b, const step_t *s)
{
    uint32_t r = step_ratchet(s) - 1u, nd = (uint32_t)step_nudge(s) & 15u, j;
    uint32_t n = s->n > 4u ? 4u : s->n, tm = s->time > ST_REST ? ST_REST : s->time;
    for (j = 0; j < 4u; j++)
        b[j] = (uint8_t)((s->note[j] > 127u ? 127u : s->note[j]) | (nd >> j & 1u) << 7);
    b[4] = (uint8_t)(n | tm << 3 | (s->flags & 3u) << 5);
    b[5] = (uint8_t)((s->vel > 127u ? 127u : s->vel) | (r & 1u) << 7);
    b[6] = s->hit;
    b[7] = s->acc & s->hit;
    b[8] = (uint8_t)((s->probability >= COND_FIRST + NCOND ? 101u : s->probability) | (r >> 1) << 7);   /* (1.5.5: 102.. a condition) */
}
static int pb_unpack_step(step_t *s, const uint8_t *b)
{
    uint32_t meta = b[4], nd = 0, j;
    for (j = 0; j < 4u; j++) {
        nd |= (uint32_t)(b[j] >> 7) << j;
        s->note[j] = b[j] & 127u;
    }
    s->n = meta & 7u;
    s->time = (meta >> 3) & 3u;
    s->flags = (meta >> 5) & 3u;
    s->vel = b[5] & 127u;
    meta |= (uint32_t)(b[5] & 128u) << 1;
    s->hit = b[6];
    s->acc = b[7];
    s->probability = b[8] & 127u;
    meta |= (uint32_t)(b[8] & 128u) << 2;
    if ((meta & 255u) > 127u || s->n > 4u || s->time > ST_REST || s->probability >= COND_FIRST + NCOND || (s->acc & ~s->hit))
        return 0;
    step_set_ratchet(s, (meta >> 8) + 1u);
    step_set_nudge(s, (int32_t)(nd ^ 8u) - 8);
    return 1;
}
static void pb_pack_pat(uint8_t *d, const step_t *st)
{
    uint32_t i;
    for (i = 0; i < NSTEP; i++)
        pb_pack_step(d + i * PB_STEP_B, &st[i]);
}
static int pb_unpack_pat(step_t *st, const uint8_t *d)   /* 0: a step is no step: every one empty */
{
    uint32_t i;
    for (i = 0; i < NSTEP; i++)
        if (!pb_unpack_step(&st[i], d + i * PB_STEP_B)) {
            for (i = 0; i < NSTEP; i++)
                step_clear(&st[i]);
            return 0;
        }
    return 1;
}

/* ---- sequences as lists ---- */
static uint8_t pb_e[PB_STEP_B];             /* the empty step, packed */
static uint8_t pb_e_ok;
static void pb_e_init(void)
{
    step_t z;
    memset(&z, 0, sizeof z);
    step_clear(&z);
    pb_pack_step(pb_e, &z);
    pb_e_ok = 1;
}
static void pb_pat_empty(uint8_t *pat)
{
    uint32_t i;
    if (!pb_e_ok)
        pb_e_init();
    for (i = 0; i < NSTEP; i++)
        memcpy(pat + i * PB_STEP_B, pb_e, PB_STEP_B);
}
/* a step's nine bytes without its note bytes when it has no notes (n = 0: they mean nothing; the nudge bits stay), so that an
 * emptied step (step_clear leaves its notes) is as empty as one never used */
static void pb_canon(uint8_t *c, const uint8_t *b)
{
    uint32_t j;
    memcpy(c, b, PB_STEP_B);
    if ((b[4] & 7u) == 0u)
        for (j = 0; j < 4u; j++)
            c[j] &= 128u;
}
static uint32_t pb_pop(uint32_t m)
{
    uint32_t n = 0;
    while (m) {
        n += m & 1u;
        m >>= 1;
    }
    return n;
}
static uint32_t pb_step_mask(const uint8_t *b)       /* which bytes differ from the empty step's (0: none, the step is empty) */
{
    uint32_t k, m = 0;
    if (!pb_e_ok)
        pb_e_init();
    for (k = 0; k < 7u; k++)
        if (b[k] != pb_e[k])
            m |= 1u << k;
    if (b[7] != pb_e[7] || b[8] != pb_e[8])
        m |= 128u;
    return m;
}
static uint32_t pb_mask_len(uint32_t m) { return 1u + pb_pop(m & 127u) + ((m & 128u) ? 2u : 0u); }
static uint32_t pb_step_put(uint8_t *d, const uint8_t *b, uint32_t m)
{
    uint32_t k, n = 1u;
    for (k = 0; k < 7u; k++)
        if ((m >> k) & 1u)
            d[n++] = b[k];
    if (m & 128u) {
        d[n++] = b[7];
        d[n++] = b[8];
    }
    d[0] = (uint8_t)m;
    return n;
}
static void pb_step_get(uint8_t *b, const uint8_t *d)   /* the mask byte at d, its bytes checked to be there */
{
    uint32_t k, n = 1u;
    memcpy(b, pb_e, PB_STEP_B);
    for (k = 0; k < 7u; k++)
        if ((d[0] >> k) & 1u)
            b[k] = d[n++];
    if (d[0] & 128u) {
        b[7] = d[n++];
        b[8] = d[n++];
    }
}
static uint32_t pb_seq_size(const uint8_t *pat)
{
    uint32_t i, n = 1u + PB_BM, m;
    uint8_t c[PB_STEP_B];
    for (i = 0; i < NSTEP; i++) {
        pb_canon(c, pat + i * PB_STEP_B);
        if ((m = pb_step_mask(c)) != 0u)
            n += pb_mask_len(m);
    }
    return n;
}
static uint32_t pb_seq_put(uint8_t *d, const uint8_t *pat)
{
    uint32_t i, n = 1u + PB_BM, m;
    uint8_t c[PB_STEP_B];
    d[0] = (uint8_t)PB_BM;
    memset(d + 1, 0, PB_BM);
    for (i = 0; i < NSTEP; i++) {
        pb_canon(c, pat + i * PB_STEP_B);
        if ((m = pb_step_mask(c)) != 0u) {
            d[1u + (i >> 3)] |= (uint8_t)(1u << (i & 7u));
            n += pb_step_put(d + n, c, m);
        }
    }
    return n;
}
/* a list -> pat; the bytes it took, 0 when it is none (too short, a bitmap of no size or too big) */
static uint32_t pb_seq_get(uint8_t *pat, const uint8_t *d, uint32_t avail)
{
    uint32_t nb, i, n;
    uint8_t tmp[PB_STEP_B];
    pb_pat_empty(pat);
    if (avail < 1u || d[0] == 0u || d[0] > 16u || 1u + d[0] > avail)
        return 0;
    nb = d[0];
    n = 1u + nb;
    for (i = 0; i < nb * 8u; i++)
        if ((d[1u + (i >> 3)] >> (i & 7u)) & 1u) {
            if (n >= avail || n + pb_mask_len(d[n]) > avail)
                return 0;
            pb_step_get(tmp, d + n);
            n += pb_mask_len(d[n]);
            if (i < NSTEP)
                memcpy(pat + i * PB_STEP_B, tmp, PB_STEP_B);
        }
    return n;
}


/* ---- the sequences not playing: RAM (1.9) ----
 * A sequence has 128 steps (1.8.6: 64) and a track eight: 32 x 1152 bytes dense would take 36 KB of the pool, which has no
 * room. They are kept as the lists the flash holds them in (the codec above: a bitmap and the bytes of the steps that
 * differ from an empty one): an empty sequence costs nothing, a sequence of a few notes a few dozen bytes. One arena holds
 * the lists of all of them, one after the other in the order of q; a change of one moves the lists behind it. The arena is
 * smaller than the 8 x 3.8 KB the flash can hold, and a sequence that does not fit it is refused (pb_select: 4;
 * a store: the sequence is not kept). The playing sequence of a track is not in it: its steps are the track's. */
#ifndef PB_ARENA_B
#define PB_ARENA_B 7168u
#endif
static uint8_t pb_arena[PB_ARENA_B] __attribute__((section(".pool")));
static uint16_t pb_asz[PB_NSEQ];         /* the size of q's list in the arena, 0: nothing in it (an empty sequence) */
static void pb_amove(uint8_t *dst, const uint8_t *src, uint32_t n)   /* (the firmware's libc has no memmove: forward or backward by where it goes) */
{
    uint32_t i;
    if (dst < src)
        for (i = 0; i < n; i++)
            dst[i] = src[i];
    else
        for (i = n; i > 0u; i--)
            dst[i - 1u] = src[i - 1u];
}
static uint32_t pb_aoff(uint32_t q)
{
    uint32_t i, o = 0;
    for (i = 0; i < q; i++)
        o += pb_asz[i];
    return o;
}
static uint32_t pb_aused(void) { return pb_aoff(PB_NSEQ); }
/* sequence q of the arena -> pat (dense) */
static void pb_mget(uint32_t q, uint8_t *pat)
{
    if (pb_asz[q] == 0u)
        pb_pat_empty(pat);
    else
        (void)pb_seq_get(pat, pb_arena + pb_aoff(q), pb_asz[q]);
}
/* pat (dense) -> sequence q of the arena; 0 when it does not fit (nothing is changed) */
static int pb_mput(uint32_t q, const uint8_t *pat)
{
    uint32_t n = pb_seq_size(pat), off = pb_aoff(q), used = pb_aused();
    if (n <= 1u + PB_BM)                       /* (nothing in it) */
        n = 0;
    if (used - pb_asz[q] + n > PB_ARENA_B)
        return 0;
    if (n != pb_asz[q])
        pb_amove(pb_arena + off + n, pb_arena + off + pb_asz[q], used - off - pb_asz[q]);
    if (n)
        (void)pb_seq_put(pb_arena + off, pat);
    pb_asz[q] = (uint16_t)n;
    return 1;
}
static void pb_mempty(uint32_t q) { uint8_t e[PB_PAT_B]; pb_pat_empty(e); (void)pb_mput(q, e); }
static uint32_t pb_phash[NTRK];           /* a hash of the playing sequence's packed steps as pb_sync last saw them (a change: dirty) */
static uint32_t pb_hash_pat(const uint8_t *b)
{
    uint32_t i, h = 2166136261u;
    for (i = 0; i < PB_PAT_B; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}
/* sequence k of track t as the list a store holds: the playing one from the track's steps, the others from the arena */
static uint32_t pb_list_put(uint8_t *d, uint32_t t, uint32_t k)
{
    uint8_t b[PB_PAT_B];
    uint32_t q = t * PB_N + k;
    if (k == pb_cur[t]) {
        pb_pack_pat(b, trk[t].step);
        return pb_seq_put(d, b);
    }
    if (pb_asz[q] == 0u) {
        d[0] = (uint8_t)PB_BM;
        memset(d + 1, 0, PB_BM);
        return 1u + PB_BM;
    }
    memcpy(d, pb_arena + pb_aoff(q), pb_asz[q]);
    return pb_asz[q];
}
static uint32_t pb_list_size(uint32_t t, uint32_t k)
{
    uint8_t b[PB_PAT_B];
    uint32_t q = t * PB_N + k;
    if (k == pb_cur[t]) {
        pb_pack_pat(b, trk[t].step);
        return pb_seq_size(b);
    }
    return pb_asz[q] ? pb_asz[q] : 1u + PB_BM;
}
static void pb_rehash(uint32_t t)         /* the track's steps as they are are the ones the store holds */
{
    uint8_t b[PB_PAT_B];
    pb_pack_pat(b, trk[t].step);
    pb_phash[t] = pb_hash_pat(b);
}

static void pb_clear_all(void)
{
    uint32_t q;
    step_t e[NSTEP];
    memset(e, 0, sizeof e);
    for (q = 0; q < NSTEP; q++)
        step_clear(&e[q]);
    memset(pb_asz, 0, sizeof pb_asz);                   /* (every sequence of the arena empty) */
    memset(pb_mo, 0, sizeof pb_mo);
    memset(pb_cur, 0, sizeof pb_cur);
    memset(pb_next, 0, sizeof pb_next);
    memset(pb_cmask, 0, sizeof pb_cmask);
    memset(pb_fired, 0, sizeof pb_fired);
    memset(pb_last, 0, sizeof pb_last);
    for (q = 0; q < PB_NSEQ; q++)
        pb_len[q] = (uint8_t)TP[P_SLEN].def;
    pb_ready = 1;
}
static void pb_need(void) { if (!pb_ready) pb_clear_all(); }

/* the playing sequence of track t into the mirror (its steps, its LEN); marks the live store's objects that changed */
static void pb_sync(uint32_t t)
{
    uint8_t b[PB_PAT_B];
    uint32_t q, h;
    pb_need();
    q = t * PB_N + pb_cur[t];
    pb_pack_pat(b, trk[t].step);
    h = pb_hash_pat(b);
    if (h != pb_phash[t]) {                             /* (the playing sequence is the track's: only that it changed is kept) */
        pb_phash[t] = h;
        pb_dirty |= (uint8_t)(1u << (q / PB_N));
        pb_gen++;
    }
    if (pb_len[q] != (uint8_t)trk[t].p[P_SLEN]) {
        pb_len[q] = (uint8_t)trk[t].p[P_SLEN];
        pb_dirty |= 1u << PB_META_O;
        pb_gen++;
    }
}
static void pb_sync_all(void)
{
    uint32_t t;
    for (t = 0; t < NTRK; t++)
        pb_sync(t);
}

/* a sequence with a note or a hit in it (the playing one: the track's steps) */
static int pb_filled(uint32_t t, uint32_t slot)
{
    uint32_t i;
    pb_need();
    if (slot == pb_cur[t])
        return !seq_is_empty(&trk[t]);
    if (pb_asz[t * PB_N + slot] == 0u)
        return 0;
    {
        uint8_t pat[PB_PAT_B];
        pb_mget(t * PB_N + slot, pat);
        for (i = 0; i < NSTEP; i++) {
            const uint8_t *b = pat + i * PB_STEP_B;
            if ((b[4] & 7u) || b[6])
                return 1;
        }
    }
    return 0;
}

/* track t plays sequence `slot`. 0 done; 1 no such; 2 the playing sequence has more than PB_MO_MAX automation records
 * (it could not be kept); 3 the automation of the sequence does not fit with the other tracks'; 4 the arena (pb_arena) has no room for
 * the sequence that was playing */
static int pb_select(uint32_t t, uint32_t slot)
{
    track_t *tr;
    motion_store_t mo, in;
    step_t ns[NSTEP];
    uint8_t pat[PB_PAT_B], cur[PB_PAT_B];
    uint32_t q, old, i, f;
    if (t >= NTRK || slot >= PB_N)
        return 1;
    pb_need();
    if (slot == pb_cur[t])
        return 0;
    tr = &trk[t];
    motion_snapshot_track(tr, &mo);
    if (mo.count > PB_MO_MAX)
        return 2;
    q = t * PB_N + slot;
    old = t * PB_N + pb_cur[t];
    memset(&in, 0, sizeof in);
    in.count = pb_mo[q].count;
    in.on = pb_mo[q].on ? (uint8_t)(1u << t) : 0u;
    for (i = 0; i < in.count && i < PB_MO_MAX; i++)
        pb_ev_to(&in.event[i], t, &pb_mo[q].ev[i]);
    if (in.count > PB_MO_MAX || !motion_valid(&in))
        memset(&in, 0, sizeof in);
    for (i = 0; i < in.count; i++)
        if (MO_TRK(in.event[i].place) != t)
            memset(&in, 0, sizeof in);
    pb_sync(t);
    pb_mget(q, pat);                                    /* the sequence to play (empty: nothing in the arena) */
    pb_pack_pat(cur, tr->step);                         /* the one playing now takes its place in the arena */
    pb_mempty(q);
    if (!pb_mput(old, cur)) {                           /* (a bank full of lists: nothing is done) */
        (void)pb_mput(q, pat);
        return 4;
    }
    (void)pb_unpack_pat(ns, pat);
    if (motion_replace_track(tr, &in)) {
        pb_mempty(old);                                 /* (undone) */
        (void)pb_mput(q, pat);
        return 3;
    }
    pb_mo[old].count = mo.count;
    pb_mo[old].on = (uint8_t)(mo.on != 0u);
    for (i = 0; i < mo.count; i++)
        pb_ev_from(&pb_mo[old].ev[i], &mo.event[i]);
    pb_mo[q].count = 0;
    f = motion_guard();
    memcpy(tr->step, ns, sizeof ns);
    tr->p[P_SLEN] = (int16_t)clamp((int32_t)pb_len[q], TP[P_SLEN].min, TP[P_SLEN].max);
    tr->rh_n = tr->rskip_n = 0;
    motion_unguard(f);
    pb_rehash(t);
    pb_cur[t] = (uint8_t)slot;
    pb_next[t] = 0;
    pb_dirty |= (uint8_t)(1u << t | 1u << PB_META_O);
    pb_gen++;
    return 0;
}

/* CHAIN: the sequences the user chained (pb_cmask, PRESETS on SEQ > SEQUENCE) of a track play one after the other, in
 * order, the last back to the first, a sequence at the end of each pass of the one playing. Only those: a sequence not chained
 * plays by itself, looping, and so does the only one chained. Whole sequences of 1..64 steps each: 8 x 64 steps at most */
static uint32_t pb_chain_count(uint32_t t)             /* how many sequences are chained, 0 when fewer than two */
{
    uint32_t m = pb_cmask[t % NTRK], n = 0;
    while (m) {
        n += m & 1u;
        m >>= 1;
    }
    return n > 1u ? n : 0u;
}
static int pb_chained(uint32_t t, uint32_t slot) { return slot < PB_N && ((pb_cmask[t % NTRK] >> slot) & 1u); }
static void pb_chain_set(uint32_t t, uint32_t slot, int on)   /* chain (or not) sequence slot of track t */
{
    uint8_t m;
    t %= NTRK;
    pb_need();
    if (slot >= PB_N)
        return;
    m = on ? (uint8_t)(pb_cmask[t] | 1u << slot) : (uint8_t)(pb_cmask[t] & ~(1u << slot));
    if (m != pb_cmask[t]) {
        pb_cmask[t] = m;
        pb_dirty |= 1u << PB_META_O;
        pb_gen++;
    }
}
static int pb_chain_next(uint32_t t)                   /* the chained sequence after the one playing; -1: it is no chained one, or alone */
{
    uint32_t c = pb_cur[t % NTRK], i;
    if (!pb_chain_count(t) || !pb_chained(t, c))
        return -1;
    for (i = 1; i < PB_N; i++)
        if (pb_chained(t, (c + i) % PB_N))
            return (int)((c + i) % PB_N);
    return -1;
}

/* the sequence a knob step starts from: the one waiting (SEQ SWITCH END) if there is one, else the one playing. Stepping from
 * the playing one made every step land on the same neighbour while a switch waited: a far sequence could not be reached */
static uint32_t pb_target(uint32_t t)
{
    t %= NTRK;
    return pb_next[t] ? (uint32_t)pb_next[t] - 1u : pb_cur[t];
}

/* a sequence chosen for track t (SEQ > SEQUENCE's knobs, SEQ held + C5..G5). MENU > CONTROL > SEQ SWITCH: NOW at once; END,
 * while the music plays, at the end of the sequence playing (pb_due). 0 switched, 1 waits, 2 it is the one playing (a
 * waiting switch is called off), < 0 pb_select's rc, negated */
static int pb_request(uint32_t t, uint32_t s)
{
    int rc;
    t %= NTRK;
    pb_need();
    if (s == pb_cur[t]) {
        pb_next[t] = 0;
        return 2;
    }
    if ((ui_prefs3 & (PREF_SEQ_END >> 24)) && song.playing && s < PB_N) {
        pb_next[t] = (uint8_t)(s + 1u);
        return 1;
    }
    rc = pb_select(t, s);
    return rc ? -rc : 0;
}
/* main loop, every pass, per track: at the end of the sequence playing, a waiting switch (SEQ SWITCH END) or else the next
 * sequence of the chain happens: on the last step (the next step is the new one's first), at once for a waiting one when
 * the music is stopped, and late (a wrap seen) when a pass missed the last step. 1 switched, 0 nothing (yet), < 0
 * pb_select's rc, negated (the switch is dropped) */
static int pb_due(uint32_t t)
{
    const track_t *tr = &trk[t];
    uint32_t idx = tr->seq_idx, run = song.playing && tr->seq_pos < 0x7FFFFFFFu, last = pb_last[t], at_end;
    int rc, nx;
    pb_last[t] = (uint8_t)idx;
    if (!run)
        pb_fired[t] = 0;                                /* (stopped: the next start is a new pass) */
    if (pb_fired[t]) {                                  /* this boundary was handled: wait for the pass to be under way */
        if (idx + 1u < (uint32_t)tr->p[P_SLEN])
            pb_fired[t] = 0;
        return 0;
    }
    at_end = run && (idx + 1u >= (uint32_t)tr->p[P_SLEN] || idx < last);
    if (pb_next[t]) {
        if (run && !at_end)
            return 0;
        rc = pb_select(t, (uint32_t)pb_next[t] - 1u);
        pb_next[t] = 0;
        pb_fired[t] = (uint8_t)run;
        return rc ? -rc : 1;
    }
    nx = at_end ? pb_chain_next(t) : -1;
    if (nx < 0)
        return 0;
    rc = pb_select(t, (uint32_t)nx);
    pb_fired[t] = 1;
    return rc ? -rc : 1;
}

/* the music was loaded (project.c project_restore_runtime): every track plays sequence 1 of an empty bank, the loaded
 * steps and LEN in it. A store (patbank_store.c) fills the rest */
static void pb_reset_to_live(void)
{
    uint32_t t;
    pb_clear_all();
    gen_defaults();                                      /* (the ARP's GEN knobs: the store's, if it has them, come after) */
    cpk_defaults();                                      /* (CHRD PLAY's chord packs, the same) */
    memset(play_lat, 0, sizeof play_lat);
    for (t = 0; t < NTRK; t++)
        pb_sync(t);
    pb_dirty = PB_ALLOBJ;
    pb_gen++;
}

/* ---- SCL > CHORDS (chord.c): the pack the black keys follow, edited and kept ---- */
static void cpk_touch(void)                             /* the packs changed: saved with the music (patbank_store.c) */
{
    pb_dirty |= 1u << PB_META_O;
    pb_gen++;
}
/* SCL > CHORDS: KNOB 1 the pack (it is loaded at once), 2 the black key, 3 its action, 4 the strum speed */
static void cpk_edit(uint32_t slot, int32_t steps)
{
    if (slot == 0u) {
        cpk_sel = (uint8_t)clamp((int32_t)cpk_sel + steps, 0, (int32_t)CPK_N - 1);
        cpk = *cpk_pack(cpk_sel);
        memset(play_lat, 0, sizeof play_lat);              /* (a pack of its own: nothing latched from the last one) */
    } else if (slot == 1u)
        cpk_key = (uint8_t)clamp((int32_t)cpk_key + steps, 0, (int32_t)CPK_KEYS - 1);
    else if (slot == 2u)
        cpk.act[cpk_key] = (uint8_t)clamp((int32_t)cpk.act[cpk_key] + steps, 0, (int32_t)CA_N - 1);
    else
        cpk.strum = (uint8_t)((uint32_t)clamp((int32_t)CPK_SPEED(&cpk) + steps, 0, (int32_t)CPK_STRUM_MAX) | (cpk.strum & CPK_LATCH));
    cpk_touch();
}
/* PRESETS on SCL > CHORDS: LATCH on (turned right) or off (left): the black keys toggle their actions instead of being held */
static void cpk_latch_ui(int32_t steps)
{
    cpk.strum = (uint8_t)((cpk.strum & ~CPK_LATCH) | (steps > 0 ? CPK_LATCH : 0u));
    memset(play_lat, 0, sizeof play_lat);
    ui_say("LATCH ", steps > 0 ? "ON" : "OFF");
    cpk_touch();
}
/* OCT+: what the black keys do now, kept as the user's pack picked (a built in one is not written over) */
static void cpk_save(void)
{
    char nb[8], mb[16] = "SAVED ";
    if (cpk_sel < CPK_NF) {
        ui_message("PICK A USER PACK");
        return;
    }
    cpk_user[cpk_sel - CPK_NF] = cpk;
    str_cpy(mb + 6, cpk_name(cpk_sel, nb), 9);
    ui_message(mb);
    cpk_touch();
}
#else
#define pb_gen 0u
#endif
