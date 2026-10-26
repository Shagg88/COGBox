# 62. Callbacks built in code (lis+addi scan)

Tip from the Gears 3 port (2026-10-22): some function addresses are built directly in code (`lis rX,hi` + `addi rY,rX,lo`)
and passed on, not stored in any table, so neither discovery nor `tools/scan_vtables.py` sees them. If the game later
calls one, the run stops on "[FATAL] Call to invalid or unregistered function".

## Tool
`tools/scan_lis_addi.py <generated dir> <loaded image>` - reads every generated file, follows each lis/addi pair in a
function, and keeps addresses inside the code range that are not function starts. It then sorts them by what happens
next (table load = a switch table; passed in r3-r10 to a `bl`; stored; ...) and whether any generated function covers
the address. It prints manifest lines for the ones in a gap (no code generated there) that are passed on or stored and
decode as code.

## Result on the job 219 generated code [V from the code and the image]
- 4,779 code addresses are built this way; 4,149 are function starts (fine), 630 are not.
- 507 of the 630 are switch tables (indexed loads), 83 more are inside a function and not passed on (scope tables and
  the like), 4 are jump-table words in gaps.
- 17 are real code in a gap, passed to a call or stored: ten 16-byte vtable-call thunks in a row at 0x829FB450-0x829FB4E0
  (each passed to a different registration call), 4 more thunks / small functions (0x82361F78, 0x828229B8,
  0x82DC0D90, 0x82D3D598), a setter (0x82A68CD0) and two functions at 0x82BADC78 / 0x82BADC88.
- Check of the method: of the 15 manifest hints that were found earlier by running the game and hitting the FATAL,
  3 (0x82362E98, 0x82D3D5C8, 0x82BFA4C0) are built this way; the rest come from data tables (vtable scan).
- [G] None of the 17 has been hit in the tests so far (29-chapter sweeps, menus, Horde, multiplayer maps), so they are
  probably on paths not tested yet. Adding them is additive (no code is generated at those addresses now).
- Not added: two addresses inside functions that are passed on (0x82A91638, a label inside its own function, and
  0x82CD7F14, the return address after a call - a resume point handed to sub_82F4D45C); a function start there would
  split working code, so only if a FATAL ever names them.

## Status
The 17 went in with job 221 (all 17 now function starts); job 222's 29-chapter sweep runs on that build.

## The Gears 3 vtable-scanner rules on Gears 2 (2026-10-22) [V from the image]
Gears 3 added two rules to our `tools/scan_vtables.py`: (1) in tables where fewer than half the entries are known
starts, take an unknown entry when the word before it ends a function (blr / bctr / b); (2) reject SEH resume points
(an `mr r8,r8` marker within 16 bytes). Run on the Gears 2 image: 1 new candidate, 0x82B32490 (a this-adjusting thunk,
`addi r3,r3,-8; b`, in a gap after the 2-instruction sub_82B32488, listed in a table at 0x82043638) - added as a hint
(18 in all with the lis+addi ones). Rule 2 rejected 6 entries that are SEH resume points. Both rules are now in our
`tools/scan_vtables.py`; with the hint added it finds no more candidates.
