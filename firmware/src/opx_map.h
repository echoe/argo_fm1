/* SPDX-License-Identifier: GPL-3.0-only */
/* The OPX engine (eng_opx.c, opx_store.c): where its data lives in flash. This header is the ONLY place the location is
 * written down; storage.c's sector map derives the objects' addresses from it.
 *
 * Argo: the sixth object of each sequence bank store. A store has room for six objects, OBJ_PB0 + 6 s + o, and has used five
 * since the sparse format (patbank_store.c, 1.8.7): the sixth, 8 KiB (an A/B pair of 4 KiB sectors), is free in every one of the
 * five stores (project A..D, the live music). OPX object i is store i's sixth: 0xAA000, 0xB6000, 0xC2000, 0xCE000, 0xDA000.
 * A store of the old format (1.8.6 and before) has its meta object there: patbank_store.c converts it (power-on) before
 * OPX may write (opx_store.c checks: pb_store_sixth_free).
 *
 * FELUCCA_OPX=0 builds without any of it: no OPX object exists, the sixth objects stay unused. */
#ifndef OPX_MAP_H
#define OPX_MAP_H
#include <stdint.h>

#ifndef FELUCCA_OPX
#define FELUCCA_OPX 1
#endif
#define FELUCCA_OPX_ONLY 0                   /* (the OPX fork's OPX-only firmware is not part of Argo) */

#define OPX_EXT_OBJS 5u                      /* one extension per project (4) and one for the autosave */
#define OPX_EXT_TAG0 0x4F50u                 /* "PO" little-endian: 0.0.1..0.1.0's commit record type of object i: + i */
/* since 0.2 (opx_store.c): objects 0, 1 two projects' patches each, 2, 3 the OPX user presets', 4 the autosave's (its
 * type kept). New types for the new contents: an object written by 0.1.0 is never read as one of 0.2 */
#define OPX_OBJ_TAG(i) ((i) == 4u ? OPX_EXT_TAG0 + 4u : 0x4F58u + (i))

/* object i's sector (copy 0 / 1): the sixth object of sequence bank store i (storage.c's pb address: 0xA0000 + 8 KiB an object,
 * six to a store) */
#define OPX_OBJ_ADDR(i, copy) (0x000A0000u + (6u * (i) + 5u) * 0x2000u + (copy) * 0x1000u)
_Static_assert(OPX_OBJ_ADDR(OPX_EXT_OBJS - 1u, 1u) + 0x1000u <= 0xDC000u, "OPX objects: inside the sequence banks' flash, below the user presets");
#endif
