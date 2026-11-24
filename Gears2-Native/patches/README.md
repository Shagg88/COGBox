# SDK patches

COGBox's runtime (`rexruntime.dll`) is the rexglue SDK at tag
`nightly-20261002-bd833a2a` with these patches.
0.6.3's runtime (rexruntime 063b) is 0.6.2's plus patches 104, 106 and 107. 0.6.4's (064a) adds 108; 064b adds 110; 064c is 064b with patch 110's start-up self-check fixed (jobs 1934 / 1935).

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
- `sdk-patch83-net-rate-trace.diff` (0.6.0, after 82): with `--cogbox_net_trace`, game UDP traffic to and from each online peer is logged once a second (`cogbox_net_rate: out -> 10.254.x.y N B/s M pkt/s`, counting 28 bytes of IP/UDP header per packet as the engine does), with a running summary every 30 s: seconds with traffic, average, peak, and how many seconds came within 10% of the games' shipped 7000 B/s cap. It is how the launcher's raised network rates for Gears 2, 3 and Judgment were measured; nothing changes without the trace flag. Apply it in the SDK root (`git apply`).
- `sdk-patch84-open-matches.diff` (0.6.0, after 83): XSessionStart / XSessionEnd on a session we host mark its lobby started / ended on the server (`PATCH /api/v1/lobbies/:id` `in_progress`), so the Open matches list shows it "In progress" (join in progress puts a joiner in a bot's place); nothing is shown while a match hasn't started or its host doesn't send it. One send at a time, always the state as it is at that moment, so sends that run out of order still end on the latest state; a refused send is tried 3 times, and a start before the server has the lobby is sent once it does. Tests in `tests/unit/net/online_sessions_test.cpp`. Apply it in the SDK root (`git apply`).
- `sdk-patch89-lobby-search-privacy.diff` (0.6.1, after 84; 85-88 are the HDR, audio and display patches): the session block a hosted lobby is registered with no longer carries the PC's LAN address - its XNADDR's `ina` is zeroed and the MAC made from that IP is made from the XUID in the online marker instead. Online peers never used either (they reach the host through the marker and its virtual IP); the game's own XNADDR is unchanged, so System Link / LAN keep the real IP. The games' lobby search now signs in, so the master server can require it (`LOBBY_SEARCH_REQUIRES_SIGN_IN`, off by default) once most players are past 0.6.0. Tests in `tests/unit/net/online_sessions_test.cpp`. Apply it in the SDK root (`git apply`).
- `sdk-patch92-matchmaking-fixes.diff` (0.6.1, after 90; Flippage's patch 91, renumbered - 90 is our media-verify patch; 93 is the ping bar): **matchmaking only finds its own playlist** - a game's search returned every public lobby of that game, and the games join whatever a search returns (Xbox Live filtered by the query), so a Social Guardian search put players into a Horde lobby. The search's playlist property (`0x1000000B`) now filters the results (a lobby that names no playlist is kept). **Gears 2's host-ban watch works**: Gears 2's older UE3 keeps an object's own index at +4 (+32 in Gears 3 / Judgment), so its object table was never found and the watch never ran (no popup, no 30-minute lift: a Gears 2 player with a quit could never host versus). The settings reader also tries UE3's real 24-byte entry (id at +4) first, as Gears 2 has a lookalike table at stride 16 that read as no penalty. One `host ban watch: profile found / IsOkayToHost found / penalty` line per run shows what the watch found. Tests in `online_sessions_test.cpp` and `host_ban_test.cpp`. Apply it in the SDK root (`git apply`).
- `sdk-patch93-ping.diff` (0.6.1, after 92; Flippage's patch 90, renumbered): the ping for the FPS bar (`--cogbox_show_ping`). Every 2 s a connected peer gets a punch on a proven route, timed by its sequence number; every COGBox (0.6.0 too) answers a punch with an ack carrying that number, so the time to the ack is the round trip, smoothed (70 % old, 30 % new). `CurrentPing()` (online_startup.h) gives the bar the ping to the host of the newest session we joined (its XUID from the session's XNADDR online marker); in a party / co-op join, which makes no session here, the host whose lobby the game looked up (invite, Join friend, open match) while its game packets arrive (a failed join stops them, so the bar drops it at once); while we host, the worst ping among the players the game put in our sessions (not every connected peer). Tests in `tests/unit/net/peer_manager_test.cpp` and `online_sessions_test.cpp`. Apply it in the SDK root (`git apply`).
- `sdk-patch107-ping-join-beacon.diff` (0.6.3, after 106; in the 0.6.3 runtime; Flippage's patch 104, renumbered - 104 is the impulse-trigger patch): the FPS bar's ping after a matchmaking join. Matchmaking puts the game in a lobby picked from search results: no session is created here and no lobby is looked up, so 0.6.2 knew no host and showed `Ping -- (hosting)` (our own party). The game's connect to a peer's join beacon (TCP 8700, Gears 3 Ranked 8675) now makes that peer the join target (`OnlineSessions::OnBeaconConnect`, the peer's XUID from its virtual IP by `VirtualPeerManager::XuidOfVip`), handled as patch 93's looked-up host: shown while its game packets arrive, forgotten 30 s after they stop. Two unit tests. Apply it in the SDK root (`git apply`).
- `sdk-patch110-e0-view-placeholders.diff` (0.6.4, after 108; runtime 064b, job 1928): rexglue 34f9923 + 10acc0d ported.
  On Windows the guest 0xE0000000 view can now start at physical 0x1000 itself (VirtualAlloc2 / MapViewOfFile3
  placeholders take a 4 KiB file offset), so generated code no longer adds 0x1000 to every access from 0xE0000000. The
  layout is OPT-IN per exe: code generated with the templates in `Gears2-Native/codegen-templates` (each game's manifest
  `template_dir`, jobs 1927 / 1927b) calls `rex::memory::detail::SetE0ViewMapped(true)` from a static initializer;
  exes generated before keep the old layout on this runtime. A module of the other layout arriving later aborts with a
  clear error; start-up self-check (marker written at physical 0x1000, read back through 0xE0000000) catches a host that
  maps it wrong (older Wine); `-DREX_E0_VIEW_MAPPED=0` in the generated code keeps the old layout. Review r106. The diff also
  changes the SDK's embedded codegen templates; the copies in `Gears2-Native/codegen-templates` (with patch 111's macros) are for a code generator built without it (the test template `test/ppc_config_h.inja` stays on the old layout).
  064c (job 1935): the self-check's marker write and read are volatile - clang -O2 dropped the plain memcpy store (it
  looked dead before the page's memset) and the check failed on a correctly mapped host (jobs 1930b / 1934).
- `sdk-patch111-vector-stores.diff` (0.6.4, after 110; the code generator `rexglue.exe`, not the runtime; job 1932):
  rexglue 55dd9b0 (its stores part) ported. stvlx / stvrx are emitted as `REX_STVLX(ea, v.u8)` / `REX_STVRX(ea, v.u8)`
  instead of a byte-by-byte loop: the 16 bytes are reversed once and written with at most two overlapping memcpys.
  The macros live in the pch template (`Gears2-Native/codegen-templates/codegen/pch_h.inja`, with patch 110's half).
  Same bytes as the loop in 256,000 checked cases (and a reviewer's 61,440). Gears 3 TU6, job 1936: patch 110 0.7 %
  and 110 + 111 1.4 % less frame time than 0.6.4 without them (PGO off, three runs each, every 110 + 111 run faster).
  Needs a code generator built with it (`work\t1932\tool-T1` on the build PC) in each game's SDK prefix.
- `sdk-patch112-open-matches-matchmaking.diff` (0.6.4, the runtime, on 064c): Gears 3 Open matches join through the
  game's own matchmaking instead of an invite. The server's new `{type:'prefer', room_code}` message (sent for
  `POST /api/v1/matches/<id>/join?via=matchmaking`), or `--net_prefer_lobby=COG-XXXX` when the launcher starts the game,
  makes `OnlineSessions::Search` answer only that lobby for 3 minutes (a game started for it: up to 15 minutes, 3 from its
  first search), looked up by room code each search, so another region's match is found; a search of another playlist,
  or a lobby that closed, gets the normal results; connecting to that host's join beacon ends it. The launcher uses it
  only for a Gears 3 match not yet in progress (untested whether a host seats matchmaking mid-match). The game then
  asks the host's join beacon (TCP 8700) for a place as its own party, and the host's game picks the team - as strangers
  met on Xbox Live. Before, every Open matches joiner came in as a member of the host's party, on the host's team
  (Discord, 16 Nov). Join friend, invites and room codes keep the invite. Unit tests: 6 search cases, 1 message parse.
- `sdk-patch109-codegen-fixes.diff` (0.6.4, after 111; the code generator `rexglue.exe`, not the runtime; jobs 1941 /
  1942): three of rexglue's code generator fixes brought over. fctid / fctidz: a value of 2^63 or more saturates to the
  largest integer (c9f1938); srad / sraw: a shift of 64 / 32 or more sets CA from the sign (02a002f); vcmpbfp: a NaN sets
  both bound bits and the record form never sets CR6 bit 0 (ab61f2e). Checked against the PowerPC ISA. Gears 3 TU6
  codegen, job 1941: 164 of 459 files differ, every differing line one of these forms (554 fctid / fctidz, 145 srad /
  sraw, 271 + 271 vcmpbfp), none of the old forms left; a tool built from the tree without it gives the same code as
  before. Needs a code generator built with it (`work\t1941\tool-T2` on the build PC, sha256 95ac2e21...) in each game's
  SDK prefix (job 1942 put it there).
- `sdk-patch108-xma-edge-decoder.diff` + `sdk-patch108-ffmpeg-xmaframes.diff` (0.6.4, after 107; the two go together):
  the XMA audio decoder follows Xenia Edge's (has207, edge b9a46a3, commits 37bf631..0ce969b: multi-stream buffer
  switches, loop end frames cut at their end subframe, seeks to a subframe (DWORD1 bits 24-26 = decode_subframe, which
  rexglue read as "output padding"), the position held until a frame is out, whole stereo subframes, error / parser
  status, split frames waiting for a late buffer, passes for subframe_decode_count above 4, one-shot sounds with no
  loop, frames after a packet-boundary frame) and XMASetLoopData reads an XMA_LOOP_DATA. Review r104 fixes: Work()
  re-checks enable/allocate under the lock, Release() clears enable, Setup() logs an error once if FFmpeg lacks the
  second diff. The second diff patches the FFmpeg submodule's xmaframes decoder (from has207/FFmpeg c4f4407, 3913ff9,
  9307052, dfc5a39): output starts 192 samples in (the realignment rexglue did in its context), a cut loop-end frame
  (header bit 0) keeps the overlap of the frame before it, frames whose data ends early play, decode errors are
  reported, and a flush callback. Apply the first in the SDK root (`git apply`), the second inside the submodule
  (`git -C thirdparty/FFmpeg apply`; that file is CRLF in the Windows checkout) - and again after every submodule or
  SDK update. Needs patch 80.
- `sdk-patch117-audio-output-options.diff` (GitHub #425, the launcher's AUDIO tab; after 116; NOT BUILT into a release
  yet - needs a runtime build): three switches in `src/audio/sdl/sdl_audio_driver.cpp`, their defaults the behaviour
  before. `--audio_output_device=<name>`: the SDL playback device of that name (exact, else the first whose name
  contains it, any case), as the launcher lists them; not found = the default device and one log line with the names
  there are. A device chosen by name stays that device; patch 66's 5 s slot checks instead that it is still in SDL's
  list, and when it was unplugged the output reopens on the default device for the rest of the run (patch 66's
  following resumes; the silent sink if that fails). Empty = the default device, followed as before. `--audio_channels`: 0 automatic (5.1, stereo when the device has two channels or
  fewer), 1 mono (the stereo fold with half of each side on both channels), 2 stereo (the fold, even on a surround
  device), 6 5.1 and 8 7.1 whatever the device reports (SDL mixes down for fewer speakers); 7.1 gets the 5.1 front,
  centre and LFE as they are and each surround channel on its back and side speaker at -3 dB. `--audio_night_mode`: a
  compressor on the final mix in the SDL callback (one peak follower for all channels, 4:1 above -30 dBFS, 5 ms / 200 ms,
  +9 dB makeup, a limiter at -1 dBFS); read on every callback, so the in-game menu switches it live; off = not called
  (the output is unchanged). The mixes are in `include/rex/audio/output_mix.h`, tested in
  `tests/unit/audio/output_mix_test.cpp` (7 cases, with the device-name match; run with a stand-in for Catch2's macros, as the SDK's test build is
  off here). Apply it in the SDK root (`git apply`).
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
- `sdk-patch85-hdr-overlays.diff` (native HDR, legoliamneeson, PR #267; after 83 - first numbered 80, which the upstream-fixes patch already had, then 83, which PR #272's net-rate trace took; 84 is reserved for PR #274's open matches; NOT BUILT into a release yet - needs a runtime build): D3D12 host UI uses a separate PQ/BT.2020 pixel shader with SDR input bounds and a fixed 203-nit white. Covers FPS text, shader notes, menus and achievements, so SDR white is never written as 10,000-nit PQ white. Includes readable HLSL and an FXC SM5.1 header. Apply in the SDK root and rebuild/install the runtime; copy the new `rexruntime.dll` beside the game after closing it. SDR keeps its original shader.
- `sdk-patch86-vulkan-hdr.diff` (legoliamneeson, PR #267; after 85 - renumbered from 81; NOT BUILT into a release yet): request Vulkan swapchain colour-space support, select an advertised 10-bit ST.2084 HDR10 format, and encode host overlays to bounded PQ/BT.2020 with the matching SPIR-V shader. Reports a missing HDR10 surface instead of sending PQ to SDR. Rebuild/install the SDK runtime. Vulkan FSR also needs FSR SDK 2.4 headers and matching signed DLLs selected by the game's `GN_FFX_DIR`; this patch does not add Vulkan DLSS.
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
- `sdk-patch87-audio-endpoint-enumerator.diff` (0.6.1, after 86; NOT BUILT yet - needs a runtime build; 83 is
  Flippage's net-rate trace, 84 is reserved for his open matches (PR #274), 85 and 86 are the native-HDR patches of #267): patch 66's 5 s check for a new Windows default output device ran
  `CoInitializeEx` + `CoCreateInstance(MMDeviceEnumerator)` + `CoUninitialize` on the audio submit path every time
  (milliseconds, sometimes far more - a periodic stall). The calling thread now keeps COM and one enumerator for its
  lifetime. Apply it in the SDK root (`git apply`); it only touches `src/audio/sdl/sdl_audio_driver.cpp`.
- `sdk-patch90-media-verify-failed-blocks.diff` (0.6.1, after 89; in the 0.6.1 runtime): `XamMediaVerificationFailedBlocks`
  returns 0 (it was a stub that returned its first argument; Gears 3 then reported a dirty disc and stopped at a level
  start - GitHub #271, GitLab 5). Apply in the SDK root (`git apply`); it only touches `src/kernel/xam/xam_misc.cpp`.
- `sdk-patch88-display-gamma-type.diff` (0.6.1, after 87; NOT BUILT yet - needs a runtime build): `--kernel_display_gamma_type`
  (0 linear, 1 sRGB, 2 BT.709 = default, 3 power) and `--kernel_display_gamma_power`, as in Xenia Canary; this SDK had the
  answer of `VdGetCurrentDisplayGamma` fixed at 2 / 2.22222233. Defaults unchanged. A test switch for the Judgment brightness
  reports (GitHub #185 / #250). Apply it in the SDK root; it only touches `src/kernel/xboxkrnl/xboxkrnl_video.cpp`.
- `sdk-patch104-xbox-impulse-triggers.diff` (0.6.3, GitHub #245; after 103; in the 0.6.3 runtime): Xbox One / Series impulse triggers. `InputSystem::SetTriggerRumble(user, left,
  right, ms)` sends `SDL_RumbleGamepadTriggers` to that player's SDL pads that report trigger motors
  (`SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN`); nothing while `--vibration` is off, COGBox's UI is open or the window is
  not in front. `--pad_trigger_rumble` (off by default) also turns on SDL's raw input controller driver on Windows: SDL
  3.4's default Xbox path there is XInput, which has no trigger motors; SDL reaches them only through Windows.Gaming.Input
  or GameInput, and its raw input driver (which pairs each pad with its WGI gamepad) is the one used here - the
  stand-alone WGI and GameInput drivers are off by default as well. SDL turned the raw input driver off by default in May
  2025 (Windows could stop delivering raw controller input until a reboot), which is why the option is off by default.
  With it on, every XInput-class pad (Xbox 360, X-mode pads, Steam Input's virtual pad) moves to raw input: other SDL
  GUIDs, a combined trigger axis, no Guide and no rumble until the pad's first input (a failed rumble is sent again with
  that input), and pads added in raw input's order (split screen may differ from the ring lights). Over Bluetooth
  Windows may drive no trigger motors even where SDL reports them. Apply it in the SDK root (`git apply`).
- `sdk-patch106-save-adopt-and-key-change-copy.diff` (0.6.3, GitHub #329 "Saves deleted"; after 104, independent of it; in the
  0.6.3 runtime, PC-tested): Gears 3
  signs PlayerStorage.dat and its profile settings with SHA-1(online XUID + data), and patches 58 / 79 only re-signed
  between the offline XUID, 0 and this run's online XUID. A save signed for any other key - the online account before a
  new one (DELETE ONLINE ACCOUNT, a new #number), a deleted `identity.json` with online play off, another PC / console,
  an old backup put back or imported - was left alone; Gears 3 reset it (brightness screen, campaign progress gone,
  checkpoints kept) and wrote fresh files over it, with no copy kept. `CogPrepareGears3Saves` (called from
  `OnlineStartup`) now does, in order:
  1. **Copy:** when this run's signing key differs from the last run's (`<user dir>\cogbox-save-key`; none yet counts
     as a change), every Gears 3 / Judgment save file in place (players 2-4's `.p2`-`.p4` settings included) is copied
     to `<user dir>\_cogbox-save-backups\<nnnnnn>-<stamp>-<old key|first|unknown>-to-<new key>\` (`nnnnnn` a sequence
     number, so order never depends on the clock; the old key is always formatted from a parsed key, never the key
     file's text; built in a `.cogbox-tmp` folder and renamed when whole, the rename retried for up to ~1 s for an
     antivirus scan). No copy is made when the newest copy there already holds every one of these files byte for byte
     (a start that couldn't note its key would otherwise copy every start); a copy past sequence number 999999 takes
     the highest free number below. A file that can't be read fails the copy: no folder, tried again next start. Before
     the Judgment swap, so it holds the outgoing mode's files. Pruning (while more than 20 copies) only removes a copy
     whose every file is, with the same data (a signature may differ), in one newer copy - oldest first, never the
     oldest copy, a `first` one or the one just made - and only ever folders named exactly as above (plus that name +
     `.cogbox-tmp`, once last written more than 10 minutes ago, so another instance's copy in progress is never
     removed). Links / junctions and folders players put there are never touched; names are matched on the native path
     text (no conversion that can throw).
  2. **Verified conversions** to this run's key from 0 / the online XUID (as before), from each of the last 8 runs'
     keys (`.pre-lastkey` backup) - so a new online account from now on is a checked conversion - and from
     `--cogbox_save_adopt_key=<16 hex digits>` (an old XUID from a `LIVE emulation with online XUID` log line;
     `.pre-adoptkey` backup).
  3. **Unverified re-sign** (`--cogbox_save_adopt_foreign`, on by default) of what is still not signed for this run,
     only for a whole, plausible save: PlayerStorage.dat alone at 4 KB - 64 KB (TU6 writes 6232-6520 bytes in our and
     players' logs), not all zero; or the full set 63E83FFF + FFE + FFD: FFF exactly 1000 bytes and not all zero, FFE /
     FFD at most 1000 bytes, empty or not all zero (TU6 writes FFF at 1000 bytes in all 181 logged writes, FFE at 0-219
     and FFD at 0, all three within milliseconds); FFF not more than an hour older than FFE / FFD, and FFE / FFD written
     within an hour of each other (FFF put back from an older backup over the game's reset files is refused; FFF newer
     than FFE / FFD is normal, as COGBox's own re-signs rewrite FFF only). The checked conversions take any size. The
     data is never changed, only the 20-byte signature; the file as it was is kept as `<file>.cogbox-adopted-<stamp>`
     (never replaced). Each adoption is noted in `<user dir>\cogbox-save-adopted` (body digest, key, signature before,
     save id). For the same key, a noted body found signed with yet another key is not adopted again, unless it carries
     its pre-adoption signature (the player put that file back); and a save adopted 3 times for this key is not adopted
     again until the key changes (logged each start) - no endless copies if the game signs with a key we don't predict.
     Anything refused is logged with the reason.
  4. The patch-101 verdict per set.
  5. This run's key is noted (newest first, 8 kept) only now, and only if the copy was made or not needed, so a failed
     copy or a crash before here keeps the old key for the next start's copy and checked conversion.
  A read failure is no longer an empty file (`CogReadFile`): a set with a file that can't be read is skipped by every
  pass. Judgment's signing can't be redone: with a new online account Judgment's online copy still starts fresh, but its
  old files are in the copy of step 1. Tests in `tests/unit/net/save_resign_test.cpp`: built and run on Linux (g++ 13.3,
  `-Wall -Wextra`, with and without `-fno-char8_t`, Catch2 3.4), as root and as a normal user: 42 pass, the read-only
  case runs on Windows only (POSIX rename replaces a read-only file); MinGW syntax-checks the Windows paths. Apply it
  in the SDK root (`git apply`).
