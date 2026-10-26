# 12 - Judgment above 60 fps (Shagg 22:53 item 7; research only, no runs)

Read from the disc's own config (Coalesced_INT.bin, Xbox360-GearEngine.ini / -GearGame.ini) [C] and from what the
earlier jobs showed [V]; anything about behaviour not seen yet is marked [G].

## What limits the frame rate
- [Engine.Engine] bSmoothFrameRate=TRUE, MinSmoothedFrameRate=22, MaxSmoothedFrameRate=62 [C]. GetMaxTickRate
  (0x8255E210) loads MaxSmoothedFrameRate at 0x8255E220 and ignores bSmoothFrameRate on the 360 (job 019 E) [V].
- --gowj_max_fps: 0 = game's 62, N = N fps, -1 = MaxSmoothedFrameRate=1000 (no limit) [V, game_patches.cpp].
  Job 019: ~108 fps at 1x with the test tools on [V]. No speed or physics problem was noted in those runs, but nobody
  looked for one [G].

## What happens to game time above 60 fps
- Game tick: UE3 ticks with the real frame time (DeltaSeconds), so movement / AI / timers scale with time, not frames [G,
  standard UE3]. [Engine.GameEngine] MaxDeltaTime=0.05 [C]: a single frame never advances more than 50 ms, so the game
  slows down below 20 fps (no effect above 60).
- Rigid-body physics (PhysX 2): [Engine.Physics] NxTimeStep=0.02 (fixed 50 Hz steps), NxMaxSubSteps=3,
  NxMaxDeltaTime=0.333 [C]; [Engine.WorldInfo] MaxPhysicsSubsteps=2 [C]. With fixed steps, a high frame rate means many
  frames take 0 steps and positions are interpolated or held [G] - physics speed itself stays right, but ragdolls /
  debris may look stepped at 100+ fps (judder at 50 Hz).
- Network tick caps (NetServerMaxTickRate 30/45) matter only online [C].

## Things to check with Shagg's next uncapped session (no automatic runs)
1. Walk / roll / reload speed and cover-slide distance at 60 vs ~110 fps (should match).
2. Ragdolls, falling debris, cloth: smooth or stepping at 50 Hz?
3. Bink cut-scenes and in-engine cinematics: in sync with audio?
4. Scripted timing (Declassified mission timers, Aftermath): same wall-clock length?
5. Any frame-counted effect (muzzle flash, hit flinch, active-reload window) shorter at high fps.
If 2 looks stepped, a later option could lower NxTimeStep (e.g. 1/120 with more substeps) through the ini - only after
it is seen, and with CPU cost measured.
