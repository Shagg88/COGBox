# 04 - Static shader corpus (Gears 3)

Goal: every compiled Xbox 360 shader on the disc, as `<vs|ps>-<XXH3-64>.ucode` (Gears 2 corpus format), so our
translator can be checked against all of them before the game ever runs. Game-derived output: `out\shaders\`, never
committed.

## Where the shaders are (verified)

- `GlobalShaderCache-Xbox360.bin` (uncompressed): 226 global shaders, plain D3D shader containers
  (flags 0x102A1100 pixel / 0x102A1101 vertex). Job 004.
- Material shaders: inside each package's `SeekFreeShaderCache` export (class ShaderCache). Example: SP_OldShipyard_04_S,
  export 4,196,213 bytes at package offset 13,149,226 (package version 0x33C, 14,000 exports). Layout seen:
  - header `ffffffff 00001404 00000000 0000000a 02 0000005f 00000a0d 00000000 0000003b` (meaning of most fields not
    worked out; 0x3B = 59 = entries in the first index),
  - index: 59 x { 16-byte key, u32 0, u16 offset, u16 size } (offsets run back to back),
  - then `00000001`, u32 uncompressed size (0x5E24), u32 compressed size (0xA98), and an XMemCompress framed stream
    (`FF 5E24 0A8A` ...). Decoded: 59 shader containers at exactly the index offsets.
  - The first ~1.8 MB of the export is compressed code, the rest low-entropy material metadata.
  So the shaders are compressed twice (package chunk, then shader-code chunk), which is why job 004 found none.

## Decoder fix (verified)

GearGame.xxx blocks 340, 344-347 failed in libmspack with "bad block type". Each starts with an LZX *uncompressed*
block of odd length (e.g. 66,195 bytes, spanning frames 1-3). CAB-style LZX puts one pad byte after an odd-length
uncompressed block; XMemCompress does not: the next block header is at frame-3 offset 659 (type 2, length 64,877, and
66,195 + 64,877 = 131,072 = the block size). Even-length blocks (e.g. 341) were never affected.
Fix: `tools/lzxd-xmem-nopad.patch` (skip the pad), applied to a *copy* of lzxd.c at build time; the SDK copy is not
touched. Result: 0 undecodable blocks. (Xenia's lzxd.c has the same pad code; it only decodes XEX files, which may
never hit this case - not checked.)

## Results so far

Workspace, 8 packages (Engine, GearGame, GearGame_P, GearStart, MP_Checkout, SP_OldShipyard_04, _04_S, _P, Startup):
20,207 unique shaders (1,561 vertex, 18,646 pixel), 1.42 M instructions, 2.7 s. Census (our decoder): no new feature
classes beyond what layer 0 translates - 405 a0-relative, 2 loops, 54 oDepth, 291 cube, 4,809 kill, 52 1D fetches,
83 fetch offsets, 43 setTexLOD, 1,115 LOD bias.

"failed" counts in the tool output are candidate (size, size, frames) triples that did not decode - mostly chance
matches in other data (guess; not checked one by one).

Next: job 006 runs it over all 1,850 packages and compile-checks every shader to DXIL + SPIR-V.

## Job 006 (whole disc, 2026-10-21)

- 1,851 packages, extraction 283 s at Below Normal (12 workers), 0 undecodable blocks.
- **111,284 unique shaders (7,861 vertex, 103,423 pixel)**, 7.2 M instructions. Most come from GearGame.xxx and the
  MP maps.
- Census: nothing layer 0 lacks. Rare cases worth a look later: 4 loops, 1 MRT export (oC1, ps-6A672D98C533B15E),
  36 3D/stacked fetches (translated as Texture3D - stacked 2D arrays not handled), 3 max4, 4 filter overrides,
  150 oDepth.
- Compile check (our translator + DXC to DXIL and SPIR-V): **all 111,284 compile to both targets, 0 failures**
  (finished 02:18, 15,586 s; the summary's 111,340 includes 56 from stale job-004 shard logs). It took 4.3 h (two DXC process launches per shader). Next time: compile a sample plus
  every shader with a rare feature, or batch many shaders per DXC call.
- The job's summary also reads stale extract-12..15 / hlsl-12..15 logs from job 004 (16 workers then); ignore those.
