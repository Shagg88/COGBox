# Release pinned for this port

Decision (Shagg, 23:34): build for Title Update 4.
Gears 2's late switch). Final pin when the SHA-1 is taken (needs a job, after Shagg says go).

Title ID:      4D530A26  [S, guidebook]
Disc:          Gears of War - Judgment (USA, Europe) (En,Fr,Es,Nl,No,Da,Fi,Zh,Ko,Pl,Ru,Cs).iso
               8,738,846,720 bytes  [V, file listing 2026-10-23]
               C:\Emulation\Games\Xbox 360\Games\Gears of War - Judgment (USA, Europe) (...)\  (read-only)
Title update:  v4, Cache\TU_16L62H6_0000014000000.0000000000109, 3,264,512 bytes  [V, file listing]
               C:\Emulation\Games\Xbox 360\Games\Gears of War - Judgment (World) (v4) (250B2C78) (Title Update)\
Media ID:      3528321A  [V, XEX header] (250B2C78 is the update package's own ID, not the disc's)
XEX version:   0.0.0.3, entry point 0x82DBD420  [V, XEX header]
               Same base release as OverkillLabs3's port (their BUILDING.md: media ID 3528321A) [S]
default.xex (base, from disc): 22,855,680 bytes, SHA-1 426C5D27FF73D3FF2AF9D404ADB49E9A3DB18BF1  [V, unpack 23:25]
TU4 image (our xextool, 2026-10-24): 0x16F0000 bytes, SHA-1 BD3FEE4AFF9BAAED7C86FD24F276D7A3471F7F34 [V, 2 pages unverified: notes/03]

Notes
- Unpacked 2026-10-23 23:25 by Unpack-Disc.bat into GearsJ-Native\game\extracted (read-only):
  2365 files, 6 folders, 7,620,166,651 bytes, every size checked [V]. File list: game\file-list.txt.
- Languages on the disc: Coalesced_ CHN CZE DAN DUT ESM FIN FRA HUN INT KOR NOR POL (12) [V].
  No Russian bundle, despite "Ru" in the disc's name, and Hungarian is present though not named.
  This is the same 12 as OverkillLabs3's v0.2.1 notes, so it is probably the same release. [G]
