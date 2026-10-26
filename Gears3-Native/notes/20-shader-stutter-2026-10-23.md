# 20 - Shader / pipeline stutter, 2026-10-23 (DXBC path, async, holds)

Labels: [V] verified (log/measurement), [S] source read, [G] guess.

## Timeline of the first-visit cost (cold cache)

| Build (Gears 2 patch) | First visit | Source |
|---|---|---|
| morning (HLSL, DXC) | 92.8 s frozen in 8.5 min, worst 22.1 s | Shagg runs [V] |
| 0051/0052 (parallel compile) | worst 5.6 s, 11.3 s in first 1.5 min | 13:56 run [V] |
| 0055 DXBC first try (9df6d2f) | stuck in frame 2 (sync pipeline per draw, ~36 ms each) | 16:22 / 16:24 runs [V] |
| 0056 async (80534fb) | no freezes after load; heavy pop-in first minute | 16:43 run [V] |
| 0062 (20b37a0) | ch10: 197,878 draws skipped in frames 301-600 | Gears 2 job 358 [V] |
| 0063 load hold (83fdc9f) | ch10: 1.7 s hold, 123 skipped | job 360 [V] |
| 0064 wider hold (7d06202) | ch10 38 / ch05 143 skipped; short holds | job 361b [V] |
| 0067 hold indicator (072accd) | play exe 20:23:39 | job 111 |

Second visit (pipelines recorded, 0058 pre-create at boot): 0-41 skipped, steady frame times [V].

## What we learned

- The cost is the driver's DXBC -> GPU compile in CreateGraphicsPipelineState, linear in bytecode size: 2.37 ms/KB,
  r 0.97 (Gears 2 pipebench, job 355) [V]. Gears 3 pairs are 83-115 KB -> 100-360 ms each cold (17:08 run) [V].
- Canary's DXBC is the same size as ours (median ratio 0.92, Gears 2 built Canary 9e03494's translator) [V]. Canary is
  smooth because it skips draws, uses 75% of cores, and pre-creates its stored pipelines at title start - not because
  its pipelines are cheap. My earlier "Canary pipelines are a few ms" was wrong.
- Canary's "Pipeline creation took 1 milliseconds" log line times only the queueing [S, Gears 2 read
  pipeline_cache.cc]. Shagg's Canary (canary_experimental@534aeb64e): Gears 3 storage 1824 shaders / 1210 pipelines;
  translation 37 ms for 1179 shaders [V].
- The driver keeps each shader stage: a new pipeline whose VS and PS were already built in other pipelines costs
  0.1 ms (Gears 2 job 362c) [V]. So building each stage once before its first draw is enough.
- During a hold the game is paused and workers get the CPU: 82 ms vs 235 ms per pipeline [V, job 360].
- 12 workers beat 6 (main hold 1749 vs 2631 ms) [V].
- UE3 creates its D3D shader objects lazily at first use: hooking the UE3 shader constructors (Gears 3 sites found:
  PS 0x829642D8 before `bl 0x82AF2628`, r4 = microcode, r27 = size; VS 0x8296414C before `bl 0x82AF2910`, r4, r28)
  gives no lead time (Gears 2 0066: 0 / 1 fires in a cold boot) [V]. Hooks written, not delivered.

## Shagg's decisions today

- Stay on the DXBC path (16:26). Canary async mode (16:35). Longer holds over stuttering frames (18:33).
- First play must be near stutter free with no shipped list (18:20, reversing an 18:01 yes). No "Prepare shaders"
  button (Gears 2 0062). All fixes apply to both games (20:19).

## Next

- Prebuild at package open: Gears 3's whole-disc corpus already exists (notes/04, job 006: 111,284 shaders in 1,851
  packages, SeekFreeShaderCache export layout). Proposed to Gears 2 at 20:44: index package -> shader hashes locally,
  and on package open queue those shaders as standalone stage builds.
- Job template lesson: job 108 waited 60 min on the old job.ps1 runner's pid (jobs\heartbeat.txt "job=N" is not a
  queue job). From job 109 on, the Gears 2 idle check uses only jobs\queue (*.ps1 + heartbeat running=-).
