# 06 - Chapters and direct chapter loading

VERIFIED (read from GearGame.xxx, enum EChapterPoint, export at package offset 10,713,487; names in their enum order):
31 campaign chapters, 0-30, then CHAP_Gameover (31), CHAP_DLC_1..18 (32-49), CHAP_MAX (50). The enum groups chapters by
location name, not by the in-game act/chapter titles; _1/_2/... are the parts of one location.

Direct load: GearGame.xxx contains the string "loadcheckpoint=0?chapter=", the same URL option Gears 2 uses, and the disc
has GearGame_P.xxx. So the Gears 2 harness command should carry over (GUESS until a run confirms it):
    --ue3_launch="GearGame_P?loadcheckpoint=0?chapter=N -nostartupmovies -FIXEDSEED"
Gears 2 lesson: a chapter started this way opens PAUSED - the first A press is Resume. Time presses by frames.

| N | EChapterPoint |
|---|---|
| 0 | CHAP_One_Nightmare |
| 1 | CHAP_One_RavensNest_1 |
| 2 | CHAP_One_RavensNest_2 |
| 3 | CHAP_Two_Hanover_1 |
| 4 | CHAP_Two_Hanover_2 |
| 5 | CHAP_Two_Hanover_3 |
| 6 | CHAP_Two_Bridge |
| 7 | CHAP_Two_Wreckage |
| 8 | CHAP_Three_Badlands_1 |
| 9 | CHAP_Three_Badlands_2 |
| 10 | CHAP_Three_Badlands_3 |
| 11 | CHAP_Three_Badlands_4 |
| 12 | CHAP_Three_Blimp_1 |
| 13 | CHAP_Three_Blimp_2 |
| 14 | CHAP_Three_Fort_1 |
| 15 | CHAP_Three_Fort_2 |
| 16 | CHAP_Four_Convoy |
| 17 | CHAP_Four_Mercy_1 |
| 18 | CHAP_Four_Mercy_2 |
| 19 | CHAP_Four_Char_1 |
| 20 | CHAP_Four_Char_2 |
| 21 | CHAP_Four_Char_3 |
| 22 | CHAP_Four_Shipyard_1 |
| 23 | CHAP_Four_Shipyard_2 |
| 24 | CHAP_Five_Maelstrom |
| 25 | CHAP_Five_Coast_1 |
| 26 | CHAP_Five_Coast_2 |
| 27 | CHAP_Five_Azura_1 |
| 28 | CHAP_Five_Azura_2 |
| 29 | CHAP_Five_Tower_1 |
| 30 | CHAP_Five_Tower_2 |
| 31 | CHAP_Gameover |

## When to sweep (Shagg, 2026-10-22 08:35: "is a chapter sweep the smartest thing this early?")
Not yet. While boots stop early, every chapter would hit the same next stop - 31 copies of one crash for an hour of
lock time. Order: (1) jobs 016/017: title + START, three direct chapter loads; (2) fix stops one at a time on ONE
chapter until it plays for minutes; (3) a ~6-chapter smoke sweep across the acts, 30 s each (~5 min); (4) the full
31-chapter sweep (tools/chapter_sweep.ps1, three ~20-min chunks under the lock) as a regression check after big changes.

## Rule for renderer tests (Shagg, 2026-10-22 09:18)
Always test our renderer in a chapter that also has a stock-backend run with the same launch, script and shot times,
so every picture has a reference. Job 017 C (chapter 17 on ours only) was black with subtitles, but with no stock run
of chapter 17 it could not be called a bug. Reference runs so far: chapter 0 (job 017 A, run 20261022-090218) and
chapter 3 (job 017 B, run 20261022-090424), stock backend, shots every 10 s, A at swaps 300/450.
