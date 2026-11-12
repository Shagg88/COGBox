# SDK patches

COGBox's runtime (`rexruntime.dll`) is the rexglue SDK at tag
`nightly-20261002-bd833a2a` with these patches.

## Quick way

```
git clone --branch nightly-20261002-bd833a2a https://github.com/rexglue/rexglue-sdk
cd rexglue-sdk
git apply ../COGBox/Gears2-Native/patches/rexglue-sdk-full.diff
```

`rexglue-sdk-full.diff` is patches 1-45 in one file; apply `sdk-patch46-*.diff` and later on top of it in number
order (the notes below say where a patch needs a different folder or `patch -p` level). Then move the third-party submodules to the tags in `sdk-pins.txt` and configure with the options in
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
- `sdk-patch62-utf8-path-arguments.diff` (GitHub #177; after 61, NOT BUILT into a release yet - needs a runtime build): a game folder whose path has non-ASCII letters (`...\GRÁFICA ...`) failed with `--game_data_root does not exist: ...GRÃFICA...`. The path cvars hold UTF-8 text, but `ReXApp::SetupEnvironment` assigned them straight to `std::filesystem::path`, which on Windows reads a `std::string` in the ANSI code page, so every accented letter became two wrong ones. `game_data_root`, `user_data_root`, `update_data_root`, `cache_root`, `metadata_root` and `log_file` now go through `rex::to_path` (UTF-8). ASCII paths are unchanged. Apply it in the SDK root (`git apply`).
- `sdk-patch63-walk-key-modifier.diff` (GitHub #196; after 62, NOT BUILT into a release yet - needs a runtime build): with the walk key (patch 60) bound to a bare modifier such as `Control` and "Hold to walk", holding it stopped all movement. Binds match modifiers exactly, and a modifier is left out of that match only when some bind uses it as a key of its own (`g_bare_mod_mask`); the mask was built from the pad binds alone, so a held walk-key Control made `W`/`A`/`S`/`D` require no Control and go silent. The walk bind's bare modifiers are now added to the mask too. "Press to switch" was not affected (the key is up while walking). Apply it in the SDK root (`git apply`) after 60; builds with 60-63 applied. Not yet tried with a keyboard in game.
- `sdk-patch64-dlc-fast-complete.diff` (GitHub #193 / #201, Gears 3 and Gears 2; after 63, NOT BUILT into a release yet -
  needs a runtime build): at start-up the game mounts the DLC one package at a time on its game thread - an overlapped
  `XEnumerate` of one item, then an overlapped `XamContentCreate` of it - and each step sat in the deferred-overlapped
  queue's artificial 100 ms sleep (`kDeferredOverlappedDelayMillis`) before completing, ~0.23 s a package, ~2.8 s for 12.
  If the game's checkpoint load starts inside that window, the game runs it after the package in flight and does not
  ask for the rest until the main menu (log: `open root 'DLC1'` then `open/create root 'SG2_0' ... GearsCheckpoint`;
  a good start opens DLC1..DLC12 and then `SG13_0`). Lower frame caps reach the checkpoint load sooner, so 30 fps lost
  the race more often. `--cogbox_dlc_fast_complete` (default on) completes these two DLC steps (marketplace content
  only) with no delay: still deferred on the dispatch thread, in FIFO order with every other overlapped, same results
  and APC. Save / profile content and every other deferred call keep the 100 ms. `false` restores the old timing.
  Apply it in the SDK root (`git apply`).
- `sdk-patch65-mouse-side-buttons.diff` (GitHub #208; after 64, NOT BUILT into a release yet - needs a runtime build): mouse buttons 4 and 5 can be bound. SDL already reports them (`SDL_BUTTON_X1` / `X2` -> `MouseEvent::Button::kX1` / `kX2` in `window_sdl.cpp`), but the keyboard-and-mouse driver dropped them; `MnkInputDriver::OnMouseDown` / `OnMouseUp` now set `VirtualKey::kXButton1` / `kXButton2`, and `keybinds.cpp` names them `MB4` / `MB5` for `keybind_*` values. The launcher offers them only for a runtime that has the names. Apply it in the SDK root (`git apply`) after 64.
- `sdk-patch77-cogbox-net.diff` (COGBox online play, after 74; in the dev-0.6.0 test runtime, not in a release yet): the network pieces online play is built from, all unused until patch 79 turns them on. A STUN (RFC 5389) Binding Request / Response codec, so a game socket can ask a STUN server what public address its packets come from; the `CGBP` punch packet that opens and keeps open the NAT path to a peer from the game's own socket; and UPnP port mapping through miniupnpc 2.3.3 (fetched as the release tarball, pinned by SHA-256; discovery waits about 1 s and a missing gateway just fails). Unit tests in `tests/unit/net`. Apply it in the SDK root (`git apply`).
- `sdk-patch78-identity.diff` (COGBox online play, after 77; in the dev-0.6.0 test runtime, not in a release yet): the player's online identity in `identity.json` - a uuid made on first start, and the discriminator (`Name#1234`), online XUID and signaling token the master server hands out. The local profile's XUID is never changed, so saves are untouched. nlohmann/json comes from the copy inja already carries. Apply it in the SDK root (`git apply`).
- `sdk-patch79-online.diff` (COGBox online play, after 78; in the dev-0.6.0 test runtime, not in a release yet; everything is off unless `--net_online`): peers found online show to the game as virtual IPs `10.254.x.y` (a table per PC) and the path to each is punched from the game's own UDP socket, so it works behind CGNAT without UPnP and nothing is relayed. Each game socket asks the STUN server (`--net_stun_server=host:port`) for its public address every 15 s; STUN answers and punch packets are taken out of the game's receives (a non-blocking receive with nothing else waiting still fails with WSAEWOULDBLOCK). Packets the game sends before the path is open wait up to 5 s (1024 at most) and then go out through the same socket; a peer that does not answer within 10 s is lost. The player's XNADDR carries `CGB1` + online XUID + session token, `XNetXnAddrToInAddr` / `XNetInAddrToXnAddr` / `XNetGetConnectStatus` map it to and from the virtual IP, and online play uses patch 49's LAN-mode XNet answers (broadcasts stay system link only). Unit tests include two real loopback sockets punching through a fake STUN server. `--net_live_emulation` (with `--net_online`, and only once the master server has given the player an online XUID - read from `<user data>/cogbox-online/identity.json` at start-up, so a new claim counts from the next start): a signed-in profile looks signed in to Xbox LIVE, online-XUID requests get that XUID (offline ones keep the profile's), privileges are granted and searches answer "no sessions". Because Gears 3 signs PlayerStorage.dat and its profile settings with the online XUID, its saves are re-signed from 0 to that XUID at start-up (first copy kept as `<file>.pre-online`, written through `<file>.cogbox-tmp`), and back to 0 when emulation is off; patch 58's signer moved to `src/system/cogbox_save_sign.cpp` for this, its own conversion unchanged. `--cogbox_net_trace` logs every XAM app message with its buffer before and after, and the sign-in / XUID / privilege queries (`cogbox_net_trace:`). Apply it in the SDK root (`git apply`).
- `sdk-patch80-upstream-fixes.diff` (0.6.0, after 79): three small fixes ported from Xenia Edge / Canary. `NtRemoveIoCompletion` keeps the whole 64-bit timeout (as 32 bits a relative timeout never expired; Edge d287b30). `XMAInitializeContext` clears the decoder's context state before filling the context, not after (the clear also reset the read offset, so every sound started at offset 0; Edge 56e9596), with `is_stereo` from bit 0 of the 0-based channel count; `XMABlockWhileInUse` really waits for the decoder to finish with the context (it always returned at once; Edge 9906014). `MmAllocatePhysicalMemoryEx` treats protection bits with no access mode as read/write instead of refusing (Canary 838850f). Apply it in the SDK root (`git apply`).
- `sdk-patch81-online-fixes.diff` (COGBox online play, after 80; first found with a third tester on a FRITZ!Box with UPnP off): a game socket's handle is released only by closesocket - on the Xbox 360 sockets are not kernel handles, and Gears 3 closing a stale handle number destroyed the party host's TCP 8701 listener 2 ms after it opened (logged as "kept socket"). When a socket pair has no direct route, the game's UDP datagrams (up to 1450 bytes) and the stream tunnel's frames go through a route that already works (a `Dgram` tunnel frame, handed to the game socket on that port from a loopback socket labelled as the peer's port), so a strict router on one side no longer stops a join; a route out counts only once the peer acks our punch (a router can let the peer in and still drop our packets out), and a closed socket's routes are dropped because Windows reuses the handle. US-W and US-E are one region, `us` (the old names are read as `us`). Diagnostics: the per-pair UDP summary (`udp out routed / relayed / in`) logs without the trace flag, plus the Windows error when the tunnel cannot reach the game, per-port punch results and the game's listen / stop listening; TCP socket create / close lines only with `--cogbox_net_trace`. Apply it in the SDK root (`git apply`).
- `sdk-patch82-online-review-fixes.diff` (0.6.0, after 81; review of PR #254): relayed-datagram hand-off sockets are capped at 8 per peer (the least recently used is closed), closed after 60 s unused and taken out of the relay endpoint table, and never aimed at the tunnel's own port (a frame inside a frame); the stale-handle socket guard of patch 81 runs only while online play runs (`ObjectTable::EnableSocketGuard`, turned on by online startup), so offline and system link release socket handles as before. Tests: the guard test turns it on, a new test checks a normal release offline. Apply it in the SDK root (`git apply`).
- `sdk-patch59-sdl-pump-without-controllers.diff` (GitHub #108, Steam Deck; after 58, NOT BUILT yet - needs a runtime build): the SDL driver pumped SDL events only from calls on a controller it already knew, so with none known it never pumped, and under Wine / Proton SDL finds pads (XInput / DirectInput) only while pumping - the 0.3.5 Deck logs show `SDL input driver initialized` and never `SDL JoystickAdded`. `EnumerateDevices` now queues a pump at most every 0.5 s while no controller is known. Apply it in the SDK root (`git apply`).
- `sdk-patch58-g3-online-xuid-save-convert.diff` (GitHub #68, jobs 1066 / 1067; COGBox 0.4.0's runtime, after 57): the
  Gears 3 friendly-fire / "killed out of nowhere" fix, ported from Xenia Canary. `NetDll_XNetRandom` fills the buffer
  with real random bytes (a locked `std::mt19937_64`; a guest NULL is refused). `XamUserGetSigninInfo` answers "online
  XUID" with 0 for a local profile, **for Gears 3 only** (title 4D5308AB); every other game keeps the old answer, so
  its saves are untouched. Because Gears 3 signs PlayerStorage.dat and its profile settings (63E83FFF + FFE + FFD) with
  SHA-1(online XUID + data), `kernel_state.cpp` converts old COGBox saves once at start-up: a save still signed with
  the offline XUID is re-signed with 8 zero bytes (data unchanged). The first file is first copied to a fresh
  `<file>.pre-xuid0-<YYYYMMDD-HHMMSS>` (verified by size before anything is written; an older backup never blocks the
  conversion), and the new file is written to `<file>.cogbox-tmp`, flushed (`_commit` on Windows) and renamed over the
  original. A profile set with a file missing is converted as far as it goes and every file left unconverted is
  logged as a warning. Logged as `cogbox_save_convert:`. A converted save makes 0.3.x reset it, so restore
  the `.pre-xuid0*` files before going back. Made against the SDK as it was before job 1060 (`xam_net.cpp` / `xam_user.cpp` `.pre1060`, `kernel_state.cpp`
  `.pre1066`); applying it reproduces 1067's three files exactly.
  System Link (patches 49b / 50) finds and connects peers by IP address (XNADDR from the adapter IP) and makes its own
  session IDs / keys with `std::random_device`, so it uses neither XNetRandom nor any XUID. The offline XUIDs were
  already the same on every PC. What Gears 3's own session code does with online XUID 0 on both PCs is untested:
  Gears 3 System Link on this runtime needs a two-PC test.
- `sdk-patch15` to `sdk-patch37` are the separate patches that went into `rexglue-sdk.diff`. They're kept
  for reference only, so don't apply them again.
- `sdk-patch16-zpd-pairing-UNBUILT.diff` was never built. It isn't in any release.

Gears 3 and Judgment use the same runtime.
- `sdk-patch80-hdr-overlays.diff` (native HDR, after 79): D3D12 host UI uses a separate PQ/BT.2020 pixel shader with SDR input bounds and a fixed 203-nit white. Covers FPS text, shader notes, menus and achievements, so SDR white is never written as 10,000-nit PQ white. Includes readable HLSL and an FXC SM5.1 header. Apply in the SDK root and rebuild/install the runtime; copy the new `rexruntime.dll` beside the game after closing it. SDR keeps its original shader.
- `sdk-patch81-vulkan-hdr.diff` (after 80): request Vulkan swapchain colour-space support, select an advertised 10-bit ST.2084 HDR10 format, and encode host overlays to bounded PQ/BT.2020 with the matching SPIR-V shader. Reports a missing HDR10 surface instead of sending PQ to SDR. Rebuild/install the SDK runtime. Vulkan FSR also needs FSR SDK 2.4 headers and matching signed DLLs selected by the game's `GN_FFX_DIR`; this patch does not add Vulkan DLSS or XeSS.
- `sdk-patch66-audio-session-volume.diff` (GitHub #203: sound on one output device, silence on another, while the log
  shows the silent device opened normally). Windows keeps a volume-mixer slider and mute for each program on each output
  device, by the exe's path, across reboots and reinstalls to the same folder. At start, and when the Windows default
  output device changes (polled about every 5 s; SDL already moves the stream there), the runtime logs the device volume
  and this game's mixer slider, and restores a muted or 0 slider (`--audio_restore_session_volume=false` leaves it). A
  slider moved during play is not touched. Apply it in the SDK root after patch 65 (`git apply --ignore-whitespace`).
- `sdk-patch68-dlc-folders-first.diff` (GitHub #240, Gears 3 RAAM's Shadow never loads with 31 DLC packages; after 67,
  NOT BUILT yet - needs a runtime build): the reporter's log shows every DLC pass opening only the first package
  (025FCF65..., loose GearGame.<lang> / Xbox360-GearGame.ini files, no folders) and never the other 30. Marketplace
  packages that hold folders (real DLC) are now enumerated first and folder-less ones last, otherwise in name order;
  nothing is dropped. Logs `cogbox_content: patch 68: ... listed after ...` when it moves any.
  `--cogbox_dlc_folders_first=false` restores the old order. Apply it in the SDK root (`git apply`); it only touches
  `src/system/xam/content_manager.cpp` (checked to apply on top of patch 56's change there).
- `sdk-patch70-video-mode-widescreen.diff` (0.4.9, 4:3 screens; NOT BUILT yet - needs a runtime build):
  `--video_mode_widescreen=false` makes VdQueryVideoMode / XGetVideoMode answer "not widescreen" (the console's 4:3 TV
  setting). The old answer (width x 3 >= height x 4) called every 4:3 mode widescreen. Gears 2 / 3 / Judgment then make
  a 4:3 back buffer (720 x width / height wide, at least 900 / 904) when the mode is at most 720 lines, so CogBox sends
  it with `--video_mode_width=960 --video_mode_height=720`. Default true = unchanged. Apply it in the SDK root
  (`git apply`); it only touches `src/kernel/xboxkrnl/xboxkrnl_video.cpp`. See docs/plan-049-4x3.md.
