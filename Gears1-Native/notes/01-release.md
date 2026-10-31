# Release pinned for this port

Build for the disc + Title Update 5 (the final patched game, as Gears 2 / 3 / Judgment ended on their title updates).

Title ID:      4D5307D5  [V, XEX header]
Disc:          Gears of War (World) (En,Fr,De,Es,It,Zh,Ko).iso, 7,834,892,288 bytes  [V, file listing]
Media ID:      76E9DF5B  [V, XEX header] (the TU package sits in a folder of the same name)
XEX version:   0.0.0.1, entry 0x82612BF0, image 0xCE0000 bytes at 0x82000000  [V, xextool]
default.xex:   4,767,744 bytes, SHA-1 EE2F204979F9811E99C4E08B70E11DE869D005C0  [V, unpack job 668]
Base image:    SHA-1 083D7D0AB2AE1EE6D9D536BC5CA987862AD8C8F7  [V, xextool image]
Title update:  v5, TU_16L61UL_0000004000000.0000000000181, 3,723,264 bytes  [V]
default.xexp:  2,295,808 bytes, SHA-1 01E2FD390BBD96B48A80E3F3559343A397D37356, STFS blocks hash-checked  [V]
TU5 image:     0.0.0.1 -> 0.0.5.1, 0x10F0000 bytes, entry 0x829F4870, SHA-1 648E50CC82641559BBDE548CE6FAAFB23802BAFF  [V]
               (the update grows the program by 4 MB: the Annex mode and fixes)

Disc layout [V, unpack log]: 1810 files in 11 folders, 6.69 GB. UE3 2006 naming: WarGame\CookedXenon\*.xxx (1745
packages), WarGame\Config\Xenon\Cooked\Coalesced.ini (one bundle), WarGame\Localization\Coalesced.<lang> (chi deu esm
esn fra int ita jpn kor). The title update adds loose ini files (WarGame.ini, Xe-WarGame.ini, Xe-WarEngine.ini,
WarPawn*.ini, WarWeapon*.ini), text, and the Annex DLC packages.

TU5 image sections [V, funcscan]: .text 0x82180000 + 0xD294FC, BINK 0x82EA9600 + 0x10934, seven .embsec_ code
sections, .pdata 0x8213F200 (27,583 functions). Starts: 37,942; indirect-only candidates 8,041.
