# COGBox: Gears 3 occlusion and checkpoint update

October 29, 2026. The normal game executables used by COGBox have been rebuilt. The new occlusion behavior is enabled automatically for Gears 3 on the existing Direct3D 12 renderer with its split rendering thread. Both the base game and TU6 builds are updated. All five game configurations built successfully; changes remain uncommitted for review.

## Lens flares and rendering work

The old path immediately completed occlusion queries with a fake visible result. UE3 consequently drew some lights and objects which walls should hide. The experimental cached-answer path could instead reuse an older object's answer and cause visibility errors.

The new `strict` path completes each query using that issuance's actual GPU result. It pairs the separate begin/end reports, keeps the end pending while the GPU works, and guards completion against a later issuance reusing the same report. A query containing missing draws falls back to visible, so an unfinished shader/pipeline cannot incorrectly hide an object. Failed or lost queries have a bounded fallback.

Results are submitted in batches and polled while the rendering thread is idle, including when the game is waiting for a query before it can submit another frame. Completing reports also wakes the existing guest-memory wait mechanism. Gears 2 and Judgment retain their existing automatic query settings; the shared implementation is available for further testing there.

The first correct-result implementation introduced an avoidable wait for the remainder of a frame's GPU work. That version was revised to submit completed query batches before subsequent rendering work, giving results an earlier fence. Extra submissions are bounded, and results are polled periodically during drawing. This recovered the initial performance loss in the specific Gridlock test below. Later uncapped Museum testing exposed further overhead; see the subsequent frame-pacing regression report. The earlier Gridlock result does not establish performance neutrality across the games.

The investigation checked [Flippage's Gears 3 occlusion fix](https://github.com/Flippage/gears-of-war-3-recomp/blob/master/docs/KNOWN_ISSUES.md), including its separate begin/end reports and deferred GPU completion. COGBox already paired the reports; its own takeover path still published fake results, which was the relevant problem here. The implementation also follows Direct3D 12's explicit query-result resolution and fence model. [Microsoft query documentation](https://learn.microsoft.com/en-us/windows/win32/direct3d12/queries).

## Gridlock measurements

Three short runs used the same Gridlock Horde launch, fixed seed, fixed 30 Hz simulation, scripted input, 3x resolution and no image capture. Rendering was uncapped. Results below use frames 601–3,600, 3,000 frames per run. The first 600 frames are excluded to avoid initial loading. Compilation of other game builds was performed separately from these performance runs.

| Measurement | Occlusion off | Initial real-result version | Final batched version |
| --- | ---: | ---: | ---: |
| Average FPS | 116.92 | 85.68 | **117.00** |
| 99th-percentile frame time | 8.634 ms | 13.049 ms | **8.980 ms** |
| Maximum sampled frame time | 33.514 ms | 13.565 ms | 9.476 ms |
| Logged draws per frame | 1,006.90 | 816.42 | **816.42** |
| Logged renderer own time per frame | 8.283 ms | 1.572 ms | 2.673 ms |
| Measured GPU time per frame | 7.785 ms | 8.275 ms | 8.327 ms |
| Frames above 50 ms / skipped draws | 0 / 0 | 0 / 0 | 0 / 0 |

The final version draws approximately **19% fewer draw calls** and maintains essentially the same average rendering rate as the fake-visible comparison. Real query testing still adds GPU work, and the final 99th percentile is slightly higher than the off comparison. These short, stationary scripted checks do not establish an FPS gain in every campaign encounter.

In a final 300-frame interval, real query results classified approximately 51.5% of queries as hidden, with zero incomplete queries, refused queries, dropped results or stale report completions in that interval. The visibility-error estimate counters from the old cached-answer mode are not independent proof of rendering correctness in strict mode.

The user confirmed the occlusion effect in the live Gridlock test. Lights became visible again during an intentionally disabled comparison; that run was not the final setting. A final smoke test used the **normal TU6 executable with no explicit query-mode argument**, confirmed automatic strict mode, and produced real hidden results without a guest exception or device-loss report. Broader campaign and camera-pan validation remains limited.

## What the latest logs say about instant death

The latest normal 30 FPS session is `Gears2-Native/cogbox/build/Gears3/logs/20261029-095014`. Its last summary reports 30.0 FPS, and the frame CSV contains 9,030 frames. It records 26 frames above 50 ms, 22 after frame 600. The worst later frame is 152.984 ms, classified as GPU-bound, with zero shader hold and zero new shaders. A separate texture-work spike also appears. The preceding `20261029-094946` session is the shorter run before the settings restart.

Neither session reports an unhandled guest exception, device loss or nonzero PM4 parser errors. The longer session ends through normal window closing. These facts distinguish the recorded rendering stalls from a game crash, but **the logs do not record the damaging actor, health transition or Kismet action at the instant-death trigger**. They cannot prove the cause of the death.

The user's reproduction at 30 FPS makes an unlocked-framerate explanation substantially less likely. Checkpoint/encounter state is a stronger lead: [Gears 3 compatibility reports](https://github.com/xenia-canary/game-compatibility/issues/39#issuecomment-3647639991) describe scripted encounters breaking after checkpoint reloads and recovering after restarting the entire chapter. That is consistent with this report, not confirmation that the same underlying defect has been identified in COGBox.

**Restarting the chapter is the current workaround to try.** A reload of the same checkpoint may restore the problematic encounter state. No gameplay damage checks were bypassed, no god mode was enabled, and no speculative save repair was applied. Exact reproduction of the reported trigger remains unverified; diagnostic launches which loaded the wrong scene were excluded from gameplay validation.

The populated save identifies `SP_Ravens_P` and `DrudgeIntro`. Its SHA-256 still matches the copied checkpoint used for testing: the original checkpoint was left unchanged. This update contains no claimed code fix for the instant-death issue.

## Build and test status

- Gears 2 Release and Release-tu6, Gears 3 Release and Release-tu6, and Judgment Release all built successfully.
- The final unit suite passed **138 cases / 818,000 assertions**, including strict-report completion, genuine zero visibility, stale-result rejection and reused/null report guards.
- The final normal-executable smoke test accounted for all 2,512 cached Gears 3 package indexes with zero package failures. Shader translation identity and the existing stage cache remain compatible; this change does not require a new complete cold shader build.
- `git diff --check` passed. Existing unrelated compiler warnings remain.
- Only isolated test user folders were used for launches. Test-owned processes have finished. The normal launch entry point remains `Gears2-Native/cogbox/build/CogBox.exe`.

Build logs, CSV results, test captures, checkpoint copies and pre-update Gears 3 executable backups are retained under `work/optimization-round2/`. The default can be overridden explicitly with `--gow2_renderer_queries=off` for a comparison; doing so restores the old visible-through-walls behavior. No launcher setting change is needed for the normal Direct3D 12 path.
