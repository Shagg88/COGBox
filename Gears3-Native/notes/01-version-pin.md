# 01 - Version pin

| Item | Value | Source |
|---|---|---|
| Title ID | 4D5308AB | title update header [V] |
| Disc | World, media ID 58122AD6 | update folder name + update header [V] |
| Title update | "Gears of War 3 Title Update #1", STFS LIVE, 2,097,152 bytes, version 0.0.0.11 (base 0.0.0.11) | `TU_16L625B_000001C000000.000000000008B` header [V] |
| Disc default.xex | 21,938,176 bytes, SHA-1 `2450fd8e4c3cf4d772cd471785d1e38cad3888ad`; execution info: media 58122AD6, version 0.0.0.11, base 0.0.0.11, disc 1 of 1; PE name `GearGame-Xbox360-Shipping.pe`; image base 0x82000000, entry 0x82CE8EB0, image size 0x15B0000 | header parse [V] |
| XDK | 20500 (D3D9LTCG, D3DX9, XAPILIB, XBOXKRNL, XAUDIO2, XGRAPHC, XONLINE... 2.0.20500.0); compiler 16.0.11886 (VS2010 era) | static library list [V] |
| Updated default.xex | YES, the TU changes code: default.xexp 1,513,472 bytes, SHA-1 d05adc22bff6197229fc3685558ba5bed6b30abf, delta 0.0.0.11 -> **0.0.1.11**. The SDK applies it cleanly ("XEX patch applied successfully") and codegen of the patched image runs (4 unresolved calls with the base hints) | 2026-10-22 13:05 [V] |

Canary game-patches lists two Gears 3 executables: base disc (`EF4AD5E9DD1982ED`) and "TU6 Disc; TU1 XBL"
(`7775898137CE1CE3`). Which one our TU produces is checked by hash after applying it [pending].

Gears 2 comparison: Gears 2 is XDK 7645, so Gears 3's D3D library (20500) is a much later build. `layer1_xdk7645` does not
carry over as is: a new `layer1_xdk20500` profile is needed (function addresses and possibly device layout differ) [V for
the version numbers, G for how much changes].

## Which Canary patch set matches (2026-10-21 21:12)
Checked by reading the instruction at each patch address in our generated code (tools: address map built from the
generated C++ comments):
- base file "4D5308AB - Gears of War 3" (hash EF4AD5E9DD1982ED): 16x AF at 0x82969614 is `lwz r5,836(r30)` (the
  patch makes it `li r5,16`), Occlusion Query Fix at 0x827B2BD0 is `lwz r11,2700(r28)` (patch `li r11,0`) - match [V].
- TU6 file (7775898137CE1CE3): at 0x827C4F80 we have `lhz r10,14(r10)`, not a `lwz r11` - no match [V].
So our disc XEX is Canary's **base** version, and every address in the base patch file applies to it.
The title update file the user has ("Title Update #1", STFS version 0.0.0.11) is not TU6; whether it changes code at
all is still open. Decision: build the base disc XEX first (D7).

## Title Update #1 contents (2026-10-22 13:00) [V]
Read with a small STFS reader (scratch tool; block layout checked against the package's own SHA-1 hash tables - every
block matches). 51 files: `default.xexp`; 46 `TU__*` at the root (gearcalendar / geargame / geargameUI text in 14
languages, `TU__gearpawn.ini`, `TU__gearweaponmp.ini`, `TU__xbox360-geargame.ini`, `TU__xbox360-gearui.ini`);
`GearGame\Patches\ScriptPatch_Engine.bin(+_us)` and `ScriptPatch_GearGame.bin(+_us)`.
Lesson: my first reader used `block > 0xAA` instead of `>= 0xAA` for the level-1 hash table, which scrambled the file
after block 170; the SDK caught it ("XEX patch block hash doesn't match", error 9).
Code moved: 73% of the image's 32-bit words differ between base and TU1, so every hint / hook / patch address must be
re-found (tools/xex_addr_map.py, shared with Gears 2).
