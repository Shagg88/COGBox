# 03 - The TU4 image, made in the cloud (2026-10-24)

Tool: tools/xextool (ours). Images are kept outside git (cloud workspace only).

| | Base disc | TU4 |
| --- | --- | --- |
| header version field | 0.0.0.3 | 0.0.4.3 |
| image | 0x16B0000 bytes at 0x82000000 | 0x16F0000 bytes at 0x82000000 (+4 pages) |
| entry point | 0x82DBD420 | 0x82DBFFD8 |
| image SHA-1 | A9A49A18135A22DC8569806BC1DA8531CA695F24 | BD3FEE4AFF9BAAED7C86FD24F276D7A3471F7F34 |
| page hash chain | all verified | 365 of 367 verified; 296-297 (0x83280000-0x8329FFFF, read-only data) not |

- [V] default.xexp from the TU package: 1,888,256 bytes, SHA-1 88E876B10276535379D5524F2BEF73ED9195870E, every
  STFS block hash-checked. 894 patch entries (673 LZX, 183 copy, 38 zero-fill).
- [V] The patch fits our disc: its image key decrypts to our base session key (the real test). The patch's own
  "source version" field says 0.0.0.9 while our header says 0.0.0.3; the field is evidently not the header
  version, so the tool warns instead of stopping.
- [V] Page hash chain, worked out here: section_digest = SHA1(range 0 + descriptor 0), and descriptor[i].digest
  = SHA1(range i+1 + descriptor i+1). 365 links hold for TU4, so the patch is applied right almost everywhere.
- [G] Pages 296-297 differ from the base in only ~140 bytes, written by four LZX entries of 0x7FE0 bytes each.
  Tried and ruled out: reading old bytes from the original image (much worse), a full-window reference (worse),
  Gears 3's LZX no-pad change (no effect). Our applier does what Xenia's ApplyPatch does, so the SDK likely
  loads the same bytes. Check on go-day: compare this SHA-1 with the SDK's own image dump. Impact for desk work:
  none (read-only data, a few bytes).
- Media ID stays 3528321A; title 4D530A26.

## Settled by job 005 (2026-10-24 09:25) [V]
- The SDK's own loaded image (--gow2_dump_image, run 20261024-092553) equals our xextool TU4 image in every 64 KB page
  except page 0 (41 bytes: the PE header the loader fills in). Pages 296-297 are identical too.
- So our applier is right, and the two page hashes that don't verify are a property of the update itself (both loaders
  produce the same bytes). Every cloud scan done on tu4.bin (notes/04, notes/05) describes the real running image.
- The dump is 0x18C1A00 bytes: PE SizeOfImage includes zero-filled data beyond the XEX image size (0x16F0000).
