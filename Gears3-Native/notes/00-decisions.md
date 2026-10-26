# 00 - Decisions log (single source of truth)

Gears 3 native port. Started 2026-10-21 ~20:45 PT from the Gears 2 handoff ("Gears 3 Native Port: Handoff and Roadmap")
and the Gears2-Native reuse snapshot (after job 179). If another note disagrees with this file, this file wins.
Status values: DECIDED (by the user), PROPOSED (my recommendation), OPEN.

## Standing rules (carried over from Gears 2, DECIDED by the user)

| # | Rule |
|---|---|
| R1 | Commit locally only; never push or share unless the user says so. |
| R2 | Author `Shagg <343598286+Shagg88@users.noreply.github.com>`, with the Co-Authored-By and Claude-Session trailers. |
| R3 | No game data in git (see `.gitignore`). |
| R4 | `C:\Emulation\Games\Xbox 360\Games` and `...\Updates` are read-only. Never delete without the user's say-so. |
| R5 | Never type into other apps; no scheduled tasks. |
| R6 | Plain English; mark verified [V] vs guessed [G]; say plainly when a theory is wrong. |
| R7 | Our code MIT; BSD/MIT code keeps its notices; GPL is ideas only; "Hollow" and the leaked-source "Delta Online" / "Project Jacinto" builds are never used. |
| R8 | References: Xenia Canary and Edge (BSD-3) for exact GPU rules; real Xbox 360 behaviour is the higher truth. |
| R9 | No helper agents on the project itself (Gears 2 D33). |
| R10 | Gears 3 never runs test jobs on the PC at the same time as Gears 2. |

## Project decisions

| # | Date | Status | Decision | Details |
|---|---|---|---|---|
| D1 | 2026-10-21 | DECIDED | Start the Gears 3 native port tonight, same architecture as Gears 2 (rexglue static recompile + our D3D12 renderer). Work starts in Claude's workspace; the PC is used read-only for the disc image and title update. | chat 20:45 |
| D2 | 2026-10-21 | PROPOSED | Pin: title 4D5308AB, World disc media 58122AD6, with Title Update #1 for that disc (STFS version 0.0.0.11). Exact XEX hashes recorded once extracted. | `01-version-pin.md` |
| D3 | 2026-10-21 | DECIDED | rexglue SDK pinned to `nightly-20261002-bd833a2a` (same as Gears 2), Gears 2 SDK patches reused from `patches/rexglue-sdk.diff`. | handoff |
| D4 | 2026-10-21 21:01 | DECIDED (user, via the Gears 2 chat) | The PC may be used for extraction, codegen, builds and analysis under three rules: own folder and queue, Below Normal priority, no game runs while Gears 2 tests (game runs need an agreed gap). Gears 2's third_party toolchain used read-only. | `02` |
| D5 | 2026-10-21 21:03 | DECIDED (user) | The handoff doc is this project's own roadmap; it lives in `02-handoff-roadmap.md` and is edited freely. | `02` |
| D6 | 2026-10-21 21:10 | DECIDED (mine, routine) | First codegen (local, pinned rexglue built with clang 20): 2 unresolved tail-call branches, fixed with function hints 0x82DB80F0 / 0x82DCE010 -> codegen clean, 459 files (169 s on 2 cores). Project = rexglue template + `GPU_PLUGINS xenos` (stock emulator GPU as the boot reference). | job 002 |
| D7 | 2026-10-21 21:13 | PROPOSED | Build the base disc XEX (SHA-1 2450fd8e...; Canary's base patch set applies). The user's Title Update #1 is looked at later; it is not Canary's TU6. | `01` |
| D8 | 2026-10-21 21:18 | DECIDED (result) | Job 002: first full build succeeded first time - codegen 70 s (459 files), compile 182 s at Below Normal on 16 threads, 0 errors, 0 warnings, gow3.exe 84 MB (stock Xenia GPU backend). No catch-funclet `end` overrides were needed (Gears 2 needed 5). | job 002 |
| D9 | 2026-10-21 21:18 | DECIDED (user) | CPU use on the PC is fine; **no GPU use** for now (no game runs). The first boot test waits for the user's go and a gap in the Gears 2 queue. | chat 21:18 |
| D10 | 2026-10-21 21:25 | DECIDED (result) | Gears 3 sets its predicated-tiling flag (0x83428B98) to 0 at startup (0x82960B94), so it renders untiled by default - no Gears 2-style untiled hook is needed. | `03` |
| D11 | 2026-10-21 21:40 | DECIDED (result) | Job 005: layered build (layer 0 copied from Gears 2, layer 2 `ue3_launch`, layer 3 Gears 3 patches) OK - compile 67 s, gow3.exe 86 MB, 0 errors; all Gears 2 unit tests pass (81 cases, 815,837 assertions). | job 005 |
| D12 | 2026-10-21 21:50 | DECIDED (result) | Material shaders are pulled from the disc packages statically (no game run needed). Two fixes, see `04-shader-corpus.md`: XMemCompress has no pad byte after odd LZX uncompressed blocks, and level shader caches are a second layer of compressed chunks. 8 packages -> 20,207 unique shaders; full disc in job 006. | `04` |
| D13 | 2026-10-22 02:18 | DECIDED (result) | Job 006: every one of the 111,284 Gears 3 shaders on the disc translates and compiles to DXIL and SPIR-V (0 failures). The shader translator needs no Gears 3 work before the first boot. | `04` |
| D14 | 2026-10-22 06:14 | DECIDED (user) | Game runs may start on their own when the Gears 2 queue has a gap: job 007 checks it read-only (no job file waiting, heartbeat idle, no gow2 process, for 10 min), runs short boot tests, and closes gow3 at once if Gears 2 becomes busy. | job 007 |
| D15 | 2026-10-22 06:51 | DECIDED (both chats, user) | Shared PC lock in use: tools\sharedpc\pclock.ps1 (in our folder until Documents\SharedPC exists - the one exception to "never write in the other folder"). Game runs, benchmarks, full builds and SDK rebuilds take it; jobs <= ~20-25 min; turn-taking; status.txt. Job 008 (gap wait) superseded by job 009 (lock). | jobs 009+, Gears 2 jobs 216+ |
| D16 | 2026-10-22 07:15 | DECIDED (result) | Gears 3 builds against its own SDK copy: third_party\rexglue-install-llvm23-rwdi-20261022-0710 (copy of Gears 2's install after their job 216: patches 1-15, 18-27; rexglue-sdk.diff sha256 93540549...). Not patch 28 (Gears 2 job 218, codegen change) - re-copy on purpose later if it measures faster. | job 010 |
| D17 | 2026-10-22 08:51 | DECIDED (user) | Gears 3 jobs check the shared lock every 15 s (Take-PcLock -PollSeconds 15) instead of the helper's 30 s default; the shared default is unchanged (Gears 2 loads the same helper). From job 017 on. | job 017 |
| D18 | 2026-10-22 09:01 | DECIDED (user) | General toolkit bugs are fixed through the Gears 2 chat as shared SDK patches (they own the SDK source); Gears 3 re-copies their install (D16) to get them. Our own SDK source copy only if Gears 3-specific toolkit changes pile up. | chat 09:01 |
