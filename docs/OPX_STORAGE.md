# Where the OPX engine keeps its data (Argo)

The OPX engine's patches are saved in five flash objects, 8 KiB each (an A/B pair of 4 KiB sectors, `storage.c`): the **sixth
object of each sequence bank store** (`opx_map.h`, `OPX_OBJ_ADDR`). A store has room for six objects; since the sparse
sequence bank format (`patbank_store.c`, 1.8.7) it uses five, so every store's sixth is free:

| OPX object | what | flash |
|---|---|---|
| 0, 1 | the tracks' patches of two projects each (a project's entry is tied to its record by a CRC) | 0xAA000, 0xB6000 |
| 2, 3 | the OPX user presets (8 each, 16 in all) | 0xC2000, 0xCE000 |
| 4 | the autosave's patches (tied to the autosave record) | 0xDA000 |

* Each OPX object has a fixed commit-record type (`OPX_OBJ_TAG`), so a sequence bank object and an OPX object that share sectors
  never read as each other.
* **A store of the old sequence bank format keeps its old meta object in the sixth object.** `pb_convert_all` converts every
  store at power-on; `opx_store.c` refuses to write (`pb_sixth_free`) while an old meta object is still there, and so does the
  backup restore of an OPX object.
* A patch is 337 bytes (`tools/opx_patch_spec.py`, format version 2): four operators.
* `FELUCCA_OPX=0` builds without the engine; the sixth objects then stay unused.
