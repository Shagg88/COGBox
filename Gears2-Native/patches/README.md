# SDK patches

COGBox's runtime (`rexruntime.dll`) is the rexglue SDK at tag
`nightly-20261002-bd833a2a` with these patches.

## Quick way

```
git clone --branch nightly-20261002-bd833a2a https://github.com/rexglue/rexglue-sdk
cd rexglue-sdk
git apply ../COGBox/Gears2-Native/patches/rexglue-sdk-full.diff
```

`rexglue-sdk-full.diff` is everything the 0.2.5 release runtime is built with: patches 1-45, in one file.
Then move the third-party submodules to the tags in `sdk-pins.txt` and configure with the options in
`sdk-cmake-options.txt`.

## What the other files are

- `rexglue-sdk.diff` is patches 1-37 combined.
- `sdk-patch38-*.diff` to `sdk-patch50-*.diff` came after it. Apply them in number order on top of it
  (there is no separate patch 49: `sdk-patch49b-system-link.diff` replaced it).
- `sdk-patch50-system-link-peers.diff` (by HubertFer, GitHub #72): with `net_peer_ip` set, System Link
  broadcasts go only to the listed PCs instead of the subnet broadcast as well (no more duplicate games in
  the Join list). With 3 or more PCs every PC must list all the others.
  Patches 39 and 40 use bare file names: apply 39 inside `src/input/mnk` (`patch -p0`) and 40 inside
  `src/audio/sdl` (`patch -p1`).
- `sdk-patch52-file-open-failed-observer.diff` (GitHub #54: a failed file open is told to an observer; Judgment's
  loading-screen cap uses it) comes after patch 49b. Apply it in the SDK root (`git apply`).
- `sdk-patch53-joystick-background-events.diff` (GitHub #108: the game's SDL input sets
  SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, as the launcher does, and rex_app.cpp no longer zeroes the pad while
  the window is unfocused) comes after patch 52. Apply it in the SDK root.
- `sdk-patch54-user-gamertag.diff` (GitHub #119): `--user_gamertag=<name>` is the gamertag player 1's profile
  shows in game (XamUserGetName / GetSigninInfo / GetGamerTag; 15 letters, digits or spaces). The profile's folder
  keeps its old name, so saves stay. Builds on patch 25 (`UserName`); apply it in the SDK root after patch 53.
  The launcher sends it from `CogBox.ini` `[Profile] Gamertag=`.
- `sdk-patch55-pad-layout.diff` (GitHub #95 / #97): controllers only, before the devices are merged -
  `--pad_radial_deadzone=<fraction>` (a round dead zone on both sticks, rescaled from its edge) and
  `--pad_remap=<src=dst,...>` (button layout; `sticks=swap` for southpaw). Apply it in the SDK root after patch 54
  (it builds on patch 38's block in `src/input/input_system.cpp`).
- `sdk-patch56-save-trace-edge-semantics.diff` (GitHub #55 #68 #79 #80 #120, Gears 3 checkpoint deaths): two switches,
  both off by default. `--cogbox_save_trace` logs content calls, save-path file opens / reads / writes, every
  overlapped completion and every failed root lookup (with the mounted roots). `--cogbox_xam_edge_semantics` switches
  content / overlapped behaviour to Xenia Edge's October 2026 series (c7a43fa ad0d187 0028503 4ee7c0f 52f802d 451f915
  af53787): the requesting thread is kept apart from InternalContext, no event with an APC, a root in use is reopened,
  TRUNCATE_EXISTING empties the package, HRESULT errors. Ported from Edge's diffs (job 1039 dumped them). Apply it in
  the SDK root after patch 55.
- `sdk-patch56b-edge-ports-from-diffs.diff`: only for an SDK that already has the first patch 56 (job 1039). It turns
  that first port, written from Edge's commit titles, into the one in `sdk-patch56`: a create on a root in use makes the
  package and fails with ERROR_DUP_NAME (it no longer closes the root), TRUNCATE_EXISTING keeps the thumbnail and refuses
  an open package, XamContentClose completes with ERROR_FUNCTION_FAILED, no raw-error change to the generic deferred
  completion, and an APC with no noted thread goes to the current one.
- `sdk-patch57-content-request-trace.diff` (after 56 / 56b): with `--cogbox_save_trace`, every XamContentCreate logs the
  request field by field (user, data size, device, type, file, the data's own xuid and title id, the profile xuid, the
  package path and whether it exists), and XMsgCompleteIORequest logs its result and length. `--cogbox_content_profile_xuid`
  (default off, an A/B): a save request whose data xuid finds no package, while the signed-in profile's does, is served
  from the profile's. Shagg's friendly-fire run opened PlayerStorage.dat on savedrive0, then failed the same package on SG0_0
  with PATH_NOT_FOUND.
  It also logs every set-information on a save file (class, value, result) and the file size after each write, and adds
  `--cogbox_save_truncate_rewrite` (default off, A/B): a write at offset 0 to a GearsCheckpoint file ends the file there.
  Gears 3 rewrites GearsCheckpoint<N>.sav from offset 0 through an OPEN_IF handle and never shortens it: Bon Voyage's
  slot 2 stayed 336,119 bytes after ~141 KB rewrites, and its bytes past the largest rewrite (0x24005) equal the older
  save's.
- `sdk-patch60-walk-key.diff` (GitHub #125; after 59, NOT BUILT yet - needs a runtime build): a keyboard walk key; see `sdk-patch60-walk-key.md`. Apply it in the SDK root (`git apply`), before 61.
- `sdk-patch61-overlay-added-files.diff` (KiltedKernel, PR #122; after 60, NOT BUILT into a release yet - needs a runtime build): a file a mod adds (one the game does not have) could never be opened through the `--ue3_mods` overlay. `VirtualFileSystem::OpenFile` checks that a cached entry still exists on the host as parent folder / name; an overlay folder is the game's own folder, so a mod's new file was not there, its entry was deleted and the open failed (`X_STATUS_NO_SUCH_FILE`; Gears 3: a mod chapter's map never loaded, GearGame_P then a loading screen forever). An entry with its own host file is now checked at that file; for a plain host folder the two paths are the same. Files a mod replaces were never affected. Verified on Gears 3 TU6: the trilogy's Gears 1 chapter loads and plays. Apply it in the SDK root (`git apply`).
- `sdk-patch59-sdl-pump-without-controllers.diff` (GitHub #108, Steam Deck; after 58, NOT BUILT yet - needs a runtime build): the SDL driver pumped SDL events only from calls on a controller it already knew, so with none known it never pumped, and under Wine / Proton SDL finds pads (XInput / DirectInput) only while pumping - the 0.3.5 Deck logs show `SDL input driver initialized` and never `SDL JoystickAdded`. `EnumerateDevices` now queues a pump at most every 0.5 s while no controller is known. Apply it in the SDK root (`git apply`).
- `sdk-patch58-g3-online-xuid-save-convert.diff` (GitHub #68, jobs 1066 / 1067; COGBox 0.4.0's runtime, after 57): the
  Gears 3 friendly-fire / "killed out of nowhere" fix, ported from Xenia Canary. `NetDll_XNetRandom` fills the buffer
  with real random bytes (a locked `std::mt19937_64`; a guest NULL is refused). `XamUserGetSigninInfo` answers "online
  XUID" with 0 for a local profile, **for Gears 3 only** (title 4D5308AB); every other game keeps the old answer, so
  its saves are untouched. Because Gears 3 signs PlayerStorage.dat and its profile settings (63E83FFF + FFE + FFD) with
  SHA-1(online XUID + data), `kernel_state.cpp` converts old COGBox saves once at start-up: a save still signed with
  the offline XUID is re-signed with 8 zero bytes (data unchanged). The first file is backed up once as
  `<file>.pre-xuid0` (verified by size before anything is written), and the new file is written to `<file>.cogbox-tmp`
  and renamed over the original. Logged as `cogbox_save_convert:`. A converted save makes 0.3.x reset it, so restore
  the `.pre-xuid0` files before going back. Made against the SDK as it was before job 1060 (`xam_net.cpp` / `xam_user.cpp` `.pre1060`, `kernel_state.cpp`
  `.pre1066`); applying it reproduces 1067's three files exactly.
  System Link (patches 49b / 50) finds and connects peers by IP address (XNADDR from the adapter IP) and makes its own
  session IDs / keys with `std::random_device`, so it uses neither XNetRandom nor any XUID. The offline XUIDs were
  already the same on every PC. What Gears 3's own session code does with online XUID 0 on both PCs is untested:
  Gears 3 System Link on this runtime needs a two-PC test.
- `sdk-patch15` to `sdk-patch37` are the separate patches that went into `rexglue-sdk.diff`. They're kept
  for reference only, so don't apply them again.
- `sdk-patch16-zpd-pairing-UNBUILT.diff` was never built. It isn't in any release.

Gears 3 and Judgment use the same runtime.
