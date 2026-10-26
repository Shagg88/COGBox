# 05 - First hook sites in the TU4 image (2026-10-24, cloud)

Method: tools/remap.py matched Gears 3's sites (its base-disc image, made with xextool from
Gears3-Native\default.xex, media 58122AD6, 0.0.0.11) against our TU4 image, and tools/xref.py found
config strings and the code that loads them. Every site below was then read in disassembly.

Matcher check [V]: Gears 3's anisotropy read 0x82969614 maps to 0x8299B564 in Judgment TU4, exactly the
address of Canary's "16x Anisotropic Filtering" patch in its TU4 file (boma). All 30 Canary TU4 patch sites
land on instructions that fit their purpose, which also backs our TU4 image (notes/03).

| Role | Gears 3 | Judgment TU4 | How found | Instruction / note |
| --- | --- | --- | --- | --- |
| Anisotropy read (16x AF) | 0x82969614 | 0x8299B564 | remap OK + Canary | `lwz r5,0x34C(r30)` |
| Tiling flag start-up store | 0x82960B94 | 0x8299257C | remap OK | `stw r10(=0),0x48A0(r5)`: stores 0 like Gears 3, so it should already run untiled [G until the first run] |
| D3D GPU-wait step (cctpl -> cctpm) | 0x82AF98EC -> 0x82AF991C | 0x82BE36D4 -> 0x82BE3704 | remap OK (same XDK code) | hook + jump_address_on_true |
| Render thread query poll head | 0x82961994 | 0x829933B4 | remap OK | `mr r4,r30` loop head |
| GetMaxTickRate, MaxSmoothedFrameRate load | 0x8253D9B0 | 0x8255E220 | Canary "Unlock FPS" site; same shape as Gears 3 | `lfs f1,0x4CC(r3)` then `blr` (Gears 3 reads 0x4C8) |
| Texture pool setup (PoolSize, then -POOLSIZE=) | 0x829677A8 | 0x82999618 | xref: "PoolSize" + "-POOLSIZE=" UTF-16 | |
| Texture pool hook (after `slwi r11,r26,20`) | 0x829678C0 | 0x82999730 | disassembly | slwi at 0x8299972C; hook the next instruction (guidebook rule) |
| Launch-data reader ("dummy " prefix) | 0x8296EEA8 | 0x829A5060 | xref: "dummy " UTF-16 (0x8218C9E0), ref at 0x829A50D0 | Gears 3's string 0x821CB974, ref 0x8296EF18 |
| VS create (XDK) | 0x82AF2910 | 0x82BE3158 | remap OK | only needed if prebuild returns |
| PS create (XDK) | 0x82AF2628 | not found | remap NOT FOUND; layout differs near VS create | only needed if prebuild returns |
| Coalesced / package hash check | (Gears 3 job 050) | wrapper 0x82DBE7E0, called from 0x8298CE74 | Canary "Disable Coalesced hash check" (Sowa_95) at 0x82DBE804 | calls XexGetModuleSection (xboxkrnl 0x196) |

## The start-up hash table is the XEX resource "HashSec" [V]
- XEX resources: "4D530A26" at 0x83593A80 (0x14839C bytes) and "HashSec" at 0x836E0000 (0x98B bytes).
- HashSec = 63 records of `lowercase name\0` + 20-byte SHA-1: coalesced_<18 languages>.bin and 45 .xxx
  packages (core, engine, startup, geargame, gfxui, loc packages...).
- Checked: SHA-1 of our disc's Coalesced_INT.bin = 7742A6C5...2BC7F5 = the coalesced_int.bin record.
- So ini edits need the matching HashSec record rewritten in guest memory after load (and the TOC lines), not a
  code patch. Gears 3's config_install.cpp does the equivalent for its 9 entries.

Canary's other TU4 sites (all reference only; most are emulator workarounds, see Gears 2 notes/40):
FPS unlock (0x8255E220 + vsync interval li r11,2 at 0x82BDF110), occlusion query 0x827DE330/0x827E2540,
lens flares 0x8288EB50, post-processing 0x827E50EC, resolution scaling 0x8236AEFC/0x8236AF3C, FXAA 0x8243FFB0,
AO 0x82428E94/0x8242AC30, motion blur 0x82441EB0/0x827CCC84, DoF 0x824D7C58/0x825B747C, bloom 0x825B7478,
film grain 0x8243CF5C, show FPS 0x83544E20 (data), 21:9 0x82992664/0x82640514/0x8261C88C.., 16:10 0x82362DB4...
