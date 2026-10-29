# Judgment lens-flare occlusion update

October 29, 2026. The normal `GearsJ-Native/build/Release/gowj.exe` used by COGBox has been rebuilt and updated.

Judgment now automatically uses the shared real-result occlusion path already validated for Gears 3, on DirectX 12 with the native split renderer. Explicit query settings are preserved. The change enables the existing implementation in Judgment's startup defaults; it does not change shader translation, resolution or flare intensity calculations, and does not require another full shader-cache rebuild.

Each query completes with its actual GPU visibility count. The shared implementation guards reused report addresses, batches submissions, polls while idle and submits query results before later frame work. Visible lights retain their flares; occluded light sources can be hidden by walls.

## Verification

The main check used **Baird's Testimony / the Museum opening**, `SP_00_Museum_P`, as requested. The initial movie-only run was excluded from occlusion validation because it exercised essentially no visibility queries. Scripted skip-button presses did not skip that movie. The subsequent isolated test used the engine's `bForceNoMovies` setting and reached playable Baird in the opening tunnel. That setting was confined to test folders.

The staged build was checked in the Museum, Academy street and an Aftermath interior. The installed normal executable was then checked in the Museum without an explicit query-mode argument, confirming that real occlusion is enabled automatically. Captures included camera turns, visible headlights and intervening geometry.

All runs below used the player's 3x resolution settings and a **30 FPS cap**, with diagnostic image capture. They are correctness checks, not uncapped performance comparisons. Initial loading is excluded from the frame samples.

| Scene / build | Frames after the first 600 | Average FPS | 99th-percentile frame time | Frames above 50 ms |
| --- | ---: | ---: | ---: | ---: |
| Museum, staged build | 1,980 | 30.01 | 35.014 ms | 0 |
| Museum, installed normal build | 930 | 30.01 | 35.168 ms | 0 |
| Academy street | 1,230 | 30.01 | 36.892 ms | 0 |
| Aftermath interior | 1,080 | 29.95 | 47.401 ms | 10 |

In the installed Museum build's final 300-frame query interval, approximately 82.2% of real query results were hidden. That interval recorded no incomplete/refused queries, stale report completions or dropped results, with about one frame of query-result latency. No checked run reported a guest exception, device loss or error-level log line. These counters demonstrate real query operation; they are not proof of perfect visibility in every scene.

Judgment Release built successfully and `git diff --check` passed. The shared query implementation is unchanged from the preceding pass, whose suite passed 138 cases / 818,000 assertions. This turn's production change is limited to Judgment's default setting.

The updated normal executable was used for the final smoke test. Test processes have finished. Existing user saves and normal movie settings were not changed; the previous executable and isolated logs/captures are retained under `work/judgment-occlusion/`.

Broader camera positions and campaign encounters remain untested, and the Aftermath check still contained hitches. This is an occlusion update, not a claim that all Judgment stuttering or pop-in is eliminated.
