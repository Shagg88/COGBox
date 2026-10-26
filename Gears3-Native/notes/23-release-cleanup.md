# 23 - Release clean-up plan (2026-10-24 04:4x, Shagg: "a deep and thorough cleaning ... before shipping")

Labels: [V] verified, [S] source read, [G] guess. Nothing changed in the working copy yet; the release is a separate,
fresh-history copy. Shagg plays tomorrow; if it goes well, he uploads.

## Scan results (253 tracked files) [V]

- No game files tracked (.gitignore covers game/, out/, generated/, *.xex, *.bin, ...).
- No secrets or tokens. No personal paths in source, except tools/sharedpc/pclock.ps1 (%USERPROFILE%\...)
  and tools/env.ps1 (mentions OneDrive).
- Do NOT ship:
  - jobs/ (126 files: PC job scripts, plus copies of Gears 2's layer 0 renderer in jobs/queue/088-files and
    pending-023).
  - notes/img/ (game screenshots = game art).
  - notes/ as a whole: chat timestamps, mailbox rules, commit trailers, game ini excerpts (e.g. the 10.1.20.20 DB
    lines), and the leaked-build (Jacinto) references.
  - patches/to-gears2/ (internal hand-offs).
  - tools/sharedpc/, tools/queue_driver.ps1, Run-Jobs.bat (PC-only job machinery).
- Git history: 258 commits with Claude-Session trailers and chat context. Release = new history, one first commit.

## Blocker: Gears 3 does not build on its own [V]

CMakeLists.txt takes layer 0 (the renderer) from ../Gears2-Native/src/layer0_xenos, and third_party from
../Gears2-Native/third_party. A public Gears 3 needs the shared renderer public too. Options for Shagg / Gears 2:
1. one repo with all games;
2. a shared "renderer" repo as a git submodule;
3. Gears 3 releases prebuilt exes only, with source later.
This is Shagg's call with the Gears 2 chat.

## Code clean-up list (src/, 1,367 lines)

- Release defaults, after tomorrow's play test (they're the measurements we want from it):
  - --gow3_tick_log 600 -> 0;
  - --gow3_memory_log_seconds 10 -> 0.
- Memory gauge thread: detached, never stops; it could touch the kernel state after shutdown [G: crash-on-exit risk].
  Give it a stop flag and join it in OnShutdown.
- Comments: drop chat timestamps ("Shagg 22:14"), job numbers, notes/NN references and the "Jacinto" wording. Keep the
  facts: addresses, what each hook does, credits to patch authors.
  - Option descriptions say "Idea from DryreL's Jacinto multipatch" -> "idea from DryreL's multipatch".
- Trilinear edits copy the game's 25 TEXTUREGROUP values with MipFilter changed (checked: identical apart from that,
  including the game's own "ansio" typo [V]). Better: change only "MipFilter=point" -> "MipFilter=linear" in whatever
  value the game has. That needs a small "replace in value" mode in layer 2's ini edits (ours). No game text in our
  source then.
- Diagnostic switches (--gow3_patch_no_light_env_shadows / no_dynamic_shadows): keep as hidden diagnostics or remove.
- gow3_app.h comments: job numbers -> plain explanations.

## Files to write for the release

README (what it is, own-disc requirement, how to play, options), LICENSE (match Gears 2's), THIRD_PARTY_NOTICES
(ReXGlue SDK, Xenia/Canary-derived code via the SDK, libmspack LGPL for tools only, XenosRecomp MIT if used,
FidelityFX/DLSS/XeSS only when built in), credits "By Shagg, with Claude (Anthropic)".
