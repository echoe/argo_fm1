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
#define PB_OLD_META_FIX (PB_HDR + 2u * PB_PAT_B + NTRK + PB_NSEQ + 2u * PB_NSEQ)
#define PB_CPK_B ((1u + CPK_USER) * (uint32_t)sizeof(cpack_t))   /* the chord packs (chord.c), after the records */
#define PB_META_RECS ((PB_OBJ_MAX - PB_META_FIX - PB_CPK_B) / 3u)
/* The object's RAM is the project staging RAM (project.c proj_wire_u): a save, a load and the autosave already take turns with
 * it, and a backup noticing proj_wire_gen knows it was reused. It is not kept a 3.8 KB buffer of its own: the pool is full */
#define pb_wire (proj_wire_u.raw)
_Static_assert(sizeof proj_wire_u.raw >= PB_OBJ_MAX, "the sequence bank objects fit the project staging RAM");
_Static_assert(GS_N <= 4u && NTRK <= 4u, "the four bytes of an object's header");
_Static_assert(PB_BM <= 16u, "a bitmap is at most 16 bytes");
#define PB_WIRE_USE() (proj_wire_gen++)

static uint32_t pb_records(void)            /* automation records the mirror holds outside the playing sequences */
{
    uint32_t t, n = 0;
    for (t = 0; t < PB_NSEQ; t++)
        if (t % PB_N != pb_cur[t / PB_N])
            n += pb_mo[t].count;
    return n;
}
static int pb_meta_ok(void) { return pb_records() <= PB_META_RECS; }

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

/* ---- objects ---- */
static uint32_t pb_track_size(uint32_t t)
{
    uint32_t k, n = PB_HDR;
    for (k = 0; k < PB_N; k++)
        n += pb_seq_size(pb_pat[t * PB_N + k]);
    return n;
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
            pos += pb_seq_put(pb_wire + pos, pb_pat[o * PB_N + k]);
        return pos;
    }
    for (q = 0; q < NTRK; q++)                          /* CHAIN: per track the mask of the chained sequences */
        pb_wire[4u + q] = pb_cmask[q];
    memcpy(pb_wire + pos, pb_cur, NTRK); pos += NTRK;
    memcpy(pb_wire + pos, pb_len, PB_NSEQ); pos += PB_NSEQ;
    for (q = 0; q < PB_NSEQ; q++) {
        int on = q % PB_N != pb_cur[q / PB_N];
        pb_wire[pos++] = on ? pb_mo[q].count : 0;
        pb_wire[pos++] = on ? pb_mo[q].on : 0;
    }
    for (q = 0; q < PB_NSEQ; q++)
        if (q % PB_N != pb_cur[q / PB_N]) {
            memcpy(pb_wire + pos, pb_mo[q].ev, pb_mo[q].count * sizeof(motion_event_t));
            pos += pb_mo[q].count * (uint32_t)sizeof(motion_event_t);
        }
    memcpy(pb_wire + pos, &cpk, sizeof cpk); pos += (uint32_t)sizeof cpk;           /* the chord packs: what the black keys do, the user's */
    for (q = 0; q < CPK_USER; q++) {
        memcpy(pb_wire + pos, &cpk_user[q], sizeof(cpack_t));
        pos += (uint32_t)sizeof(cpack_t);
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
        if ((v[2] != 1 && v[2] != 2) || len != PB_HDR + PB_PER * PB_PAT_B)
            return 0;
        off = PB_HDR + (q % PB_PER) * PB_PAT_B;
    } else {
        if (v[2] < 1 || v[2] > 3 || len < PB_OLD_META_FIX)
            return 0;
        off = PB_HDR + (q - PB_PER * PB_OLD_META_O) * PB_PAT_B;
    }
    memcpy(pat, v + off, PB_PAT_B);
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
    for (k = PB_HDR; k < n - 2u * PB_PAT_B; k++)        /* (the firmware's libc.c has no memmove: a forward copy, the destination is below the source) */
        pb_wire[k] = pb_wire[k + 2u * PB_PAT_B];
    n -= 2u * PB_PAT_B;
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
        if (pos + tot * sizeof(motion_event_t) > n) return 0;
    }
    for (t = 0; t < NTRK; t++)                          /* CHAIN: the masks */
        pb_cmask[t] = pb_wire[4u + t];
    for (q = 0; q < PB_NSEQ; q++) {
        pb_mo[q].count = 0;
        if (!cnt[q])
            continue;
        pb_mo[q].on = on[q] != 0u;
        for (k = 0; k < cnt[q]; k++, pos += sizeof(motion_event_t))
            memcpy(&pb_mo[q].ev[k], pb_wire + pos, sizeof(motion_event_t));
        pb_mo[q].count = cnt[q];
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
    for (t = 0; t < NTRK; t++) {                        /* the tracks' sequences; one that does not parse stays empty */
        n = (uint32_t)st_load(OBJ_PB0 + s * PB_NOBJ + t, pb_wire, sizeof pb_wire);
        if ((int)n >= (int)PB_HDR && pb_hdr_ok(t) && pb_track_ok(n)) {
            pos = PB_HDR;
            for (k = 0; k < PB_N; k++)
                pos += pb_seq_get(pb_pat[t * PB_N + k], pb_wire + pos, n - pos);
            for (k = 0; k < GS_N; k++)                  /* GEN's knobs */
                gen_set[t][k] = pb_wire[4u + k] < 128u ? pb_wire[4u + k] : gen_set[t][k];
        }
    }
    for (t = 0; t < NTRK; t++) {            /* the sequence a track plays is the loaded music's (the record) */
        pb_cur[t] = cur[t];
        pb_mo[t * PB_N + cur[t]].count = 0;
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
    if (cnt && cnt <= PB_MO_MAX && pos + 2u * PB_NSEQ + (skip + cnt) * sizeof(motion_event_t) <= n) {
        memcpy(mo->ev, pb_wire + pos + 2u * PB_NSEQ + skip * sizeof(motion_event_t), cnt * sizeof(motion_event_t));
        mo->count = (uint8_t)cnt;
        mo->on = pb_wire[pos + 2u * qi + 1u] != 0u;
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
