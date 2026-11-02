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
- `sdk-patch15` to `sdk-patch37` are the separate patches that went into `rexglue-sdk.diff`. They're kept
  for reference only, so don't apply them again.
- `sdk-patch16-zpd-pairing-UNBUILT.diff` was never built. It isn't in any release.

Gears 3 and Judgment use the same runtime.
