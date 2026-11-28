/* SPDX-License-Identifier: GPL-3.0-only */
/* The OPX extension objects in storage.c and the room they take (opx_map.h), against a simulated NOR.
 *   the map: 5 A/B objects, the sixth object of each sequence bank store (patbank_store.c), apart from every other object;
 *   identity: the commit record's type is the fixed tag OPX_OBJ_TAG(i); the other objects keep type = enum value; an OPX
 *   object and the sequence bank object that shares its sectors never read as each other;
 *   isolation: a save of an OPX object writes its own two sectors and nothing else.
 * Built twice by tests/run_tests.sh: FELUCCA_OPX=1 (all of the above) and FELUCCA_OPX=0 (no OPX object). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define __attribute__(x)

static uint8_t nor[0x100000];
static uint32_t writes;                              /* erases + programs */
static uint32_t w_lo = 0xFFFFFFFFu, w_hi;            /* the span written since the last reset */
static void w_note(uint32_t off, uint32_t n)
{
    writes++;
    if (off < w_lo) w_lo = off;
    if (off + n > w_hi) w_hi = off + n;
}
static void w_reset(void) { writes = 0; w_lo = 0xFFFFFFFFu; w_hi = 0; }
static int st_read(uint32_t off, void *dst, uint32_t n) { memcpy(dst, nor + off, n); return 0; }
static int st_erase(uint32_t off) { w_note(off, 4096); memset(nor + off, 0xFF, 4096); return 0; }
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    uint32_t i;
    w_note(off, n);
    if ((off & 0xFFu) + n > 256u) { printf("page wrap at %#x\n", off); exit(1); }
    for (i = 0; i < n; i++) nor[off + i] &= s[i];
    return 0;
}
#include "../firmware/src/storage.c"

static int check(const char *what, int ok)
{
    printf("opx store: %-86s %s\n", what, ok ? "ok" : "FAIL");
    return !ok;
}

int main(void)
{
    int bad = 0;
    memset(nor, 0xFF, sizeof nor);
#if FELUCCA_OPX
    {
        uint32_t o, c, i, ok = 1;
        for (i = 0; i < OPX_EXT_OBJS; i++)
            for (c = 0; c < 2u; c++) {
                /* the sixth object of store i: OBJ_PB0 + 6 i + 5 (patbank_store.c) */
                uint32_t s = st_sector(OBJ_OPX0 + i, c), pb = st_sector(OBJ_PB0 + 6u * i + 5u, c);
                ok &= s == pb && s == 0xA0000u + (6u * i + 5u) * 0x2000u + c * 0x1000u;
            }
        for (o = 0; o < OBJ_OPX0; o++)                    /* no other object shares a sector with one of ours */
            for (c = 0; c < 2u; c++)
                for (i = 0; i < OPX_EXT_OBJS; i++)
                    ok &= st_sector(o, c) != st_sector(OBJ_OPX0 + i, 0) && st_sector(o, c) != st_sector(OBJ_OPX0 + i, 1);
        for (o = 0; o < 6u * OPX_EXT_OBJS; o++)            /* nor does a sequence bank object but the sixths */
            for (i = 0; i < OPX_EXT_OBJS; i++)
                ok &= o % 6u == 5u || (st_sector(OBJ_PB0 + o, 0) != st_sector(OBJ_OPX0 + i, 0) &&
                                       st_sector(OBJ_PB0 + o, 1) != st_sector(OBJ_OPX0 + i, 1));
        bad += check("5 A/B objects, the sixth objects of the five sequence bank stores; no other object shares a sector", ok);
        ok = 1;
        for (o = 0; o < OBJ_OPX0; o++)
            ok &= st_tag(o) == o;
        for (i = 0; i < OPX_EXT_OBJS; i++)
            ok &= st_tag(OBJ_OPX0 + i) == OPX_OBJ_TAG(i) && st_tag(OBJ_OPX0 + i) >= OBJ_COUNT &&
                  (i == 4u ? st_tag(OBJ_OPX0 + i) == OPX_EXT_TAG0 + 4u : st_tag(OBJ_OPX0 + i) >= OPX_EXT_TAG0 + 5u);
        bad += check("commit record types: other objects unchanged, OPX objects fixed tags", ok);
    }
    {   /* save every OPX object: own sectors only, the tag on flash, loads back */
        static uint8_t pay[OPX_EXT_OBJS][1800], got[1800];
        uint32_t i, k, ok = 1;
        st_hdr_t h;
        for (i = 0; i < OPX_EXT_OBJS; i++) {
            for (k = 0; k < sizeof pay[i]; k++)
                pay[i][k] = (uint8_t)(k * 7u + i * 31u + 1u);
            w_reset();
            ok &= st_save(OBJ_OPX0 + i, pay[i], sizeof pay[i]) == 0;
            ok &= w_lo == st_sector(OBJ_OPX0 + i, 0) && w_hi <= st_sector(OBJ_OPX0 + i, 0) + 4096u;
        }
        bad += check("each save writes copy A of its own object only", ok);
        ok = 1;
        for (i = 0; i < OPX_EXT_OBJS; i++) {
            memcpy(&h, nor + st_sector(OBJ_OPX0 + i, 0), sizeof h);
            ok &= h.type == OPX_OBJ_TAG(i) && h.magic == ST_MAGIC;
            ok &= st_load(OBJ_OPX0 + i, got, sizeof got) == (int)sizeof got && !memcmp(got, pay[i], sizeof got);
        }
        bad += check("on flash: magic and the fixed tag; every object loads back", ok);
        /* the sequence bank object over the same sectors is not an OPX object, and the other way round */
        ok = 1;
        for (i = 0; i < OPX_EXT_OBJS; i++)
            ok &= st_load(OBJ_PB0 + 6u * i + 5u, got, sizeof got) < 0;                  /* (OPX's there: no bank object) */
        ok &= st_save(OBJ_PB0 + 5u, pay[0], 100) == 0 && st_load(OBJ_OPX0, got, sizeof got) < 0 &&
              st_load(OBJ_PB0 + 5u, got, sizeof got) == 100;
        bad += check("a sequence bank object and the OPX object in the same sectors never read as each other", ok);
    }
#else
    bad += check("FELUCCA_OPX=0: no OPX object", OBJ_COUNT == OBJ_AUTOSAVE + 1u);
#endif
    printf(bad ? "opx store test FAILED\n" : "opx store test passed\n");
    return bad != 0;
}
