# 03 - Gears 2 hook sites found in Gears 3

Method (tools/analysis, game-free scripts): both executables' images are dumped from a local rexglue build (env
`REX_DUMP_IMAGE`, workspace-only change) and disassembled with capstone; function starts come from the generated code.
`match.py` finds the Gears 3 function whose normalised instruction sequence (registers kept, numbers masked) best matches
a Gears 2 function (3-gram Jaccard prefilter, then difflib ratio), and maps the site through the alignment.
`xref.py` finds code that addresses a global (lis + d(rX)). `side.py` prints both functions aligned.
**Validation:** Gears 2's 16x AF site 0x8280E7FC maps to Gears 3 0x82969614 (ratio 0.86) = exactly the address in
Canary's Gears 3 patch [V].

| Gears 2 | Role | Gears 3 | Confidence |
|---|---|---|---|
| 0x8280E7FC `lwz r5,0x198(r30)` | anisotropy setting read (16x AF patch) | 0x82969614 `lwz r5,836(r30)` | [V] (Canary agrees) |
| 0x83122EA4 | GUseTilingMode (predicated tiling flag) | **0x83428B98** | [V] two reader functions agree (827B5A58, 82960BE8) |
| 0x82806A44 `stw r11` (stores **1** at startup) | tiling on at startup | 0x82960B94 `stw r10` with `li r10,0` - Gears 3 stores **0** | [V]: **Gears 3 runs untiled by default.** No "Black Shading Fix" needed (Canary has none for Gears 3). |
| 0x827FB7A8-7FC (in 827FB748) | console command toggling tiling | 0x8296CD9C-DF0 (in 8296CD40) | [V] same code |
| 0x829783A8 | render thread's D3D query poll (GetData) | 0x82AFA708 (ratio 0.95) | [V]-ish |
| 0x82807588 | loop calling the poll | 0x82961960 (0.78) | [G] check |
| 0x82AA8904 `li r3,1` | Coalesced hash result | 0x82CE5D1C or 0x82CE5C2C (two identical 1.0 matches) | [G] ambiguous - check callers |
| 0x827F9148 | Coalesced check function | 0x82CCE718 (0.82) | [G] |
| 0x829A0C40 / 0D78 / 00D8 / 0170 / 0E58 / 0DF0 | XGSetTextureHeaderEx / XGSetCubeTextureHeaderEx / XGSetVertexBufferHeader / XGSetIndexBufferHeader / XGOffsetResourceAddress / XGOffsetBaseTextureAddress | 0x82B093A8 / 0x82B09420 / 0x82B09078 / 0x82B09110 / 0x82B09568 / 0x82B09500 | [V]-ish (0.93-1.0) |
| 0x82976600 / 0x829768C0 | D3D lock core / unlock | 0x82AE8E40 (0.88) / 0x82AE9140 (0.48) | lock [V]-ish, unlock [G] |
| 0x8280D4C8 (PoolSize read 0x8280D534) | texture pool setup | **0x829677A8**: reads ini `PoolSize` (string 0x8212F9D8), then **command-line overrides `-POOLSIZE=` (0x821CAD60) and `-REDUCEPOOLSIZE=` (0x821CAD78)**; final MB in r26 at 0x829678BC, then `MB<<20` rounded to 4 KB | [V]. No hook needed: pass `-POOLSIZE=196` (or more) on the command line. |
| 0x8280DDC8 RHIGetTextureMemoryStats, 0x8280D848 defrag, 0x8280D150, 0x8280B0C0, 0x828093B8 | texture pool stats / defrag / mips | no reliable match yet | [open] - only needed if Gears 3 shows the Gears 2 ch18 symptom |
| 0x8280F0B0 | launch-data reader | the function calling XamLoaderGetLaunchData (recomp file 183); prefix string "dummy " at 0x821CB974 | [V] |

Strings present in Gears 3 (UTF-16): `loadcheckpoint`, `chapter=`, `BENCHMARK`, `BENCHMARKSECONDS=`, `FIXEDSEED`,
`nostartupmovies`, `PoolSize`, `bUseTextureStreaming` [V] - the Gears 2 chapter-boot command line should work.
