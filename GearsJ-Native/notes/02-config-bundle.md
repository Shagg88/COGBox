# 02 - Config bundle (Coalesced_*.bin)

Tool: tools/coalesced_dump.py (ours). Read from the unpacked disc copy, never written.

- [V] Coalesced_INT.bin: 85 ini/int files. Bundle integers are big-endian, but UTF-16 FStrings (negative length)
  are LITTLE-endian here. (Ini key names inside the XEX image are big-endian UTF-16; the bundle is not.)
- [V] Xbox360-GearEngine.ini [SystemSettings]: MotionBlur=True, MotionBlurPause=True, MotionBlurSkinning=1;
  [SystemSettingsMobile] MotionBlur=FALSE. Same keys and values as Gears 3, so the shared ue3_motion_blur switch
  proposed by Gears 3 (04:38) would cover Judgment with no Judgment-specific code.
- [V] TU4 overrides Xbox360-GearGame.ini and Xbox360-GearUI.ini (TU__xbox360-*.ini), not the engine ini, so it
  does not change the motion-blur keys.
- [S, Gears 3 06:10] Gears 3's layer 2 config_bundle.cpp already reads/writes wide FStrings little-endian and refuses
  to edit a bundle whose re-write isn't byte-identical. Run it on a Judgment bundle before relying on it.
- Hash table: Gears 2 and 3 find it by scanning the image for "<name>\0" + old SHA-1 (works whatever the section);
  in Judgment that table is the HashSec resource (notes/05).
- [V] Launcher options (Shagg 09:45, all games): Judgment has the same keys as Gears 3 -
  [FullScreenMovie] StartupMovies=Open_Logos / StartupMovies=LoadingMovie, bForceNoMovies=false;
  [SystemSettings] MaxDrawDistanceScale=1 (SplitScreen2 .8), DepthOfField=True, LensFlares=True;
  RoadieRunShakyCamDampeningFactor=0.6f in [GearGame.GearPawn] of GearPawn.ini, GearPawnMP.ini AND GearPawnFFA.ini
  (Judgment's free-for-all file), 0.45f for GearPawn_LocustKantusArmor. TU4 overrides gearpawnmp/gearpawnffa but not
  GearPawn.ini (campaign).
