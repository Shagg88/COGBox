# 07 - Chapter sweep 1 (job 013, 2026-10-24 14:14-14:28)

Job 011's exe (memexport default), our renderer, 1x, each map started straight in (--ue3_launch="<map> -nostartupmovies"),
A pressed every 5 s from 15 s, 100 s per map. Marks: [V] verified here, [G] guess.

| Map | Errors | Unregistered | Peak memory | Seen at the end |
|---|---|---|---|---|
| SP_01_University_P | 0 | 0 | 1,952 MB | gameplay, city street [V] |
| SP_02_Mansion_P | 0 | 0 | 1,956 MB | gameplay, garden courtyard [V] |
| SP_03_Island_P | 0 | 0 | 2,045 MB | gameplay, Onyx Point - Section 1 "Fortress" [V] |
| SP_04_Rooftops_P | 0 | 0 | 2,048 MB | gameplay, Downtown Halvo Bay "Wharf District" (Silverback) [V] |
| SP_05_Courthouse_P | 0 | 0 | 1,810 MB | opening cinematic, still playing at 92 s [V] |
| SPE_E1_P (Aftermath) | 0 | 0 | 1,870 MB | opening cinematic, still playing at 90 s [V] |

- Pictures clean in all six (lighting, characters, HUD, subtitles) [V].
- Cinematics show "<BACK> TO SKIP": A doesn't skip them. Next sweep: press BACK (0x0020) to skip [G], or run longer.
- First visit to a map freezes once (University: 41.5 s at frame 232): 237 pipelines made on the render thread in 42.2 s,
  "pipeline waits (budget -1 ms/frame)". Shaders are not the problem (6 waits, 1.9 s). After that: steady 30 fps.
  Pipelines are saved (build\RelWithDebInfo\out\shader-cache\pipelines-d3d12.bin), so a second visit is short [V].
  Sent to Gears 2 at 14:33 (Gears-Mailbox\to-gears2\20261024-143300-judgment-first-visit-pipeline-freezes.md),
  asked about a wait budget, using the 12 pipeline workers, and the cache location.
- Not swept yet: the transition maps (_Transition_P, SP_03_to_04_P, SP_04_to_04/05_P, SP_02_Mansion_3_P,
  SP_04_Rooftops2_P, SP_05_CourthouseBoss_P, SP_05_CourthouseTrans_P), Aftermath E2-E5, SPE_E2oilrig_P, SPE_E3_Outro_P.

# Sweep 2 (job 014, 14:32-14:46): BACK + A alternating every 5 s, shots now the game window only (PrintWindow) [V]

| Map | Errors | Unregistered | Peak memory | Seen at the end |
|---|---|---|---|---|
| SP_05_Courthouse_P | 0 | 0 | 1,924 MB | opening cinematic still at 91 s [V] |
| SP_05_CourthouseBoss_P | 0 | 0 | 2,044 MB | gameplay, burning city rooftop, "Revived by Loomis" [V] |
| SPE_E1_P | 0 | 0 | 1,991 MB | opening cinematic still at 92 s [V] |
| SPE_E2_P | 0 | 0 | 2,043 MB | gameplay, flooded dock [V] |
| SPE_E2oilrig_P | 0 | 0 | 1,939 MB | gameplay, oil rig interior [V] |
| SPE_E3_P | 0 | 0 | 1,749 MB | gameplay, King Raven outside, "Checkpoint - saving content" [V] |
| SPE_E4_P | 0 | 0 | 2,016 MB | gameplay, interior [V] |
| SPE_E5_P | 0 | 0 | 1,833 MB | gameplay, interior with light flare [V] |

- 12 of 14 swept maps reached gameplay; all 14 clean, 0 errors, 0 unregistered functions, peak 1.7-2.0 GB.
- BACK tapped for 0.2 s reached the game (log: "press 0x0020 for 0.2") but did not skip: the skip likely needs BACK
  held [G]. Courthouse and Aftermath E1 get one more short run with "BACK:2" (job 015).

# Job 015 (14:49-14:53): BACK held 2 s every 6 s - still no skip [V]
Courthouse and Aftermath E1 show the same cinematic frames at the same times as in job 014; 0 errors.
Likely reason [G]: in a chapter boot the player is signed in on controller 0, while the scripted pad is an extra
controller, and the cinematic only listens to the player's own pad (on the menus route START at the title makes the
scripted pad the player's). Not worth more PC time: the cinematics play out on their own; both maps loaded clean.
Side effect worth knowing: sweeps 1+2 filled the saved-pipeline file for all 14 maps, so Shagg's first visits through
Play.bat (same build folder) should freeze much less [G - to confirm in his log].
