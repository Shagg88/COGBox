# 04 - Finding functions before the first boot (2026-10-24, cloud, no codegen)

Tools (ours): tools/funcscan.py (entry points by source), tools/remap.py (base -> TU4 address matcher).
Images from tools/xextool (notes/03). No codegen has run, so "known to the recompiler" is approximated by
.pdata + direct-call targets.

## What the TU4 image holds [V]
- .text 0x822A0000 + 0xDFE9AC; BINK 0x8309EA00 + 0x20BB0; RADCODE 0x830BF600 + 0xAFC (three code sections).
- .pdata: 43,385 functions (start + length). Direct-call (bl) targets: 24,524.
- Indirect-only entry candidates (pointed to from data, or built with lis+addi, but never a pdata/bl start):
  13,078 (12,820 outside any pdata function, mostly tiny leaf methods with no unwind info). This is a superset:
  rexglue's own vtable/RTTI detection will already find most; the codegen log decides which are still missing.
- The ".reloc" range does not hold relocations in the loaded image (it holds other data), so pointer finding has
  to scan data words; that gives some coincidences (e.g. 0x83000000 stored as a plain number).

## Cross-check against OverkillLabs3's base-disc hints (reference only, per Shagg's 04:22 rule) [V]
- 478 addresses (433 entries + 45 Bink chunk heads with a parent).
- remap.py found 451 of them in TU4 (94%); 15 not found, 12 ambiguous (code the update changed, or tiny
  duplicate functions). Those 27 are left to our own scans and the boot loop.
- Of their 478 on the base disc: about 300 are genuinely indirect (in our candidate set), 45 are Bink jump-table
  chunks, and 103 are neither. 69 of those 103 sit within 1 KB of a 64 KB boundary (chance would be ~3%).
  So a large class of their hints is tied to 64 KB boundaries, a recompiler/codegen effect rather than a
  missing indirect call.
- Lead [S, rexglue v0.10.0 src/codegen/function_scanner.cpp ~1629/1651; mechanism G]: detectJumpTable treats a
  table or base address whose low 16 bits are 0 as "only the high half seen yet" and adds the next addi/ori it
  meets. When a switch's base label really sits on a 64 KB boundary (the compiler emits lis with no addi), a later
  unrelated addi gets folded in, the table is misread, and its targets go missing - which would put the missing
  entries right around 64 KB boundaries, as measured. To test on go-day: list every bctr whose lis/addi base is
  0x....0000 in the TU4 image and compare with the codegen's jump-table log. If confirmed it is an SDK fix for
  all three games (mail Gears 2 with the evidence).
- Tested at the desk [V]: only 9 bctr sites in the base image have a lone lis to a 64 KB boundary in code, and
  they touch 7 of the 91 boundary-near reference hints. So this explains a few at most; the lead is weak.
  Most boundary-near hints are still unexplained. Parked until the first codegen log, which names the cause.

## For go-day
- Feed the codegen our candidates in rounds rather than all 13k at once: first the ones in tables with known
  methods next to them (strong evidence), then the rest only if the boot loop asks.
- Bink: expect the same jump-table pattern they hit (auto-detection found 4 of 11 entries).
