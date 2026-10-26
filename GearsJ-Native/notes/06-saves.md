# 06 - Saves (job 012, 2026-10-24 14:09-14:14)

Marks: [V] verified here, [S] read in source/notes, [G] guess.

## What happens on a brand-new user data folder [V, job 012 run 1, out\userdata-fresh-012]
- 14:10:11 the game creates the checkpoint content (Headers\00000001\GearsCheckpoint.header) and, in the same
  frame, opens SG0_0:\GearsCheckpoint0.sav before the device is mounted:
  `VFS: 'SG0_0:\GearsCheckpoint0.sav' -> [no device]` / `ResolvePath(...) failed - device not found` (the only error).
- 14:10:12 slots 1 and 2 are created (0 bytes, unused); 14:10:21 profile + PlayerStorage.dat written;
  14:10:42 GearsCheckpoint0.sav written (164,747 bytes). So the game recovers on its own within the same run.
- No "corrupt save" message on screen (shots 31-62 s: first-time "learn the basics" prompt, campaign menu).

## Second run on the same folder [V, run 2]
- No errors. The campaign menu now offers Continue (the save is seen); the A presses went into Continue, loading
  screen, then the chapter cinematic ("to skip" prompt) - no complaint, the save loads.

## Conclusion
The first-open failure is real (the SDK mounts SG0_0: only after XamContentCreateEx finishes, the game opens
once before that) but harmless here: the save is written 30 s later and loads next time. OverkillLabs3's symptom
(saves reading as corrupted on a first run) does NOT show on our SDK build. No fix for now; watch for it in
Shagg's play test (a fresh install = an empty out\userdata). If it ever matters, the fix is SDK-side (content device
mounted before XamContentCreateEx returns) -> a patch mailed to Gears 2, not ours to change.

## Other notes
- Save folder: out\userdata\B13EBABEBABEBABE\4D530A26\00000001\ (GearsCheckpoint\, PlayerStorage.dat\) + Headers\.
- Screenshot problem seen here: the job runner's console windows (and once the taskbar) covered the game in the
  shots - run.ps1 copies the screen region. Fix: capture the window itself (PrintWindow) - see tools/run.ps1.
