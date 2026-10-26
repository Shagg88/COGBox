# 47 - Stage 4 reference rules (Brief 14 answers: occlusion queries, fences, swap, busy-wait; Canary e6cd2e4bc + Edge 8fb2772)

From the research chat, 2026-10-21 10:00. Labels: [V] verified in source, [D] documented in a comment, [I] inferred. Reference only; the 360 hardware wins (D23).

The headline: Canary now uses real host occlusion queries. Its default mode writes a quick estimate to the game's memory straight away and corrects it once the GPU's real count arrives. Fences, interrupts and the ring read pointer all become visible to the game **the moment the command packet is processed**, not when the GPU has actually finished the work.

Source is Canary `e6cd2e4bc`, paths relative to `src/xenia/`. Labels: **[V]** verified in source, **[D]** documented in a comment, **[I]** inferred.

## 1. Occlusion queries on Xenos

**How a game issues them** [V/D]
- With the `PM4_EVENT_WRITE_ZPD` (0x5B) packet (`gpu/xenos.h:1659`). The GPU writes a snapshot of its sample counters to the address in **`RB_SAMPLE_COUNT_ADDR`** (0x2325). `RB_SAMPLE_COUNT_CTL` is ignored: "unused by real hardware". Address 0 means nothing is written (`gpu/pm4_command_processor_implement.h:986–993`).
- **No per-query counter:** "Xenos doesn't have a hardware counter for each query… D3D subtracts the BEGIN report from the END report." Each `Issue` writes a running-total snapshot (`gpu/xenos_zpd_report.h:21–55`) [D].

**What gets written** (`xe_gpu_depth_sample_counts`, `gpu/xenos.h:1586–1599`) [V/D]
- Eight 32-bit values: `Total_A/B`, `ZFail_A/B`, **`ZPass_A/B`**, `StencilFail_A/B`.
- Stored **little-endian**: "swapped in D3D code". D3D sums each A/B pair.
- Counters are free-running and wrap at 32 bits. What the A and B lanes mean is unknown.

**The "done" signal** (`pm4_command_processor_implement.h:996–1000`) [D]
- While counting, D3D fills the struct with the **byte-swapped sentinel `0xFFFFFEED`**, usually in `ZPass_A/B` and in `ZFail` on some 2005–06 SDKs. The game polls until the sentinel is gone.
- That's why Canary writes the ZPass and StencilFail lanes **last, in one copy**: "GetData (usually) wakes on the ZPass lanes".

## 2. What Canary does today

| Setting | Default | Meaning |
|---|---|---|
| `occlusion_query` (`gpu/command_processor.cc:54–68`) | **`fast`** | One of `fake`, `fast`, `fast-alt`, `strict` |
| `occlusion_query_fake_lower/upper_threshold` (`gpu/gpu_flags.cc:68,74`) | 80 / 100 | Fake mode: the count cycles down from 100 to 80. "−1 writes nothing, resulting in some games that sit and hang." |

- **fast** (the default) [V]:
  - Ends the host query segment at each ZPD packet.
  - Writes an **immediate estimate**: the last real result for that address, or 1 sample if there isn't one.
  - When the real host count arrives, `PumpPendingRetire` rewrites the value and adjusts later reports (`command_processor.cc:1057–1377`).
  - A report that never resolves is forced through after 250 ms. If the query pool runs out, at least 1 is reported.
- **strict** [V]:
  - Writes nothing until the host query resolves.
  - When the game blocks (`WAIT_REG_MEM` or an empty ring), it flushes GPU work and waits for the result. The time limit is 2 ms, "with a floor of one so culling doesn't flash occluded" (`command_processor.cc:1275–1319`).
- **Known game problems** [D] (`command_processor.cc:1074–1078`):
  - Replaying zero counts "often improves correctness… but stale zeroes tend to break occlusion culling tests, resulting in popping primitives".
  - That's why **fast** treats zero as 1.

## 3. Fences and ring sync: everything is immediate on the command thread

- **`EVENT_WRITE_SHD`** (`pm4_command_processor_implement.h:906–941`) [V]:
  - Writes either the packet's value, or Canary's internal counter if bit 31 of the initiator is set, to a game address, byte-swapped according to the address's low 2 bits.
  - The counter goes up on every vblank and every swap. Xenia isn't sure of its meaning: "GPU vblank counter?" [D].
  - **D3D fences and `BlockUntilIdle` read this.** It's written as soon as the packet is processed, before the host GPU has run the earlier draws [I].
- **`MEM_WRITE`, `COND_WRITE`, `REG_TO_MEM`:** immediate writes [V]. **`EVENT_WRITE_EXT`** writes faked full-screen extents: "Let us hope we can fake this" [D].
- **`WAIT_REG_MEM`** (`:721–777`) [V]:
  - Loops on the command thread.
  - If `wait ≥ 0x100`, it sleeps `wait/0x100` ms, but only when `vsync` is on. Otherwise it spins: "User wants it fast and dangerous".
- **`INTERRUPT` packet:** calls the game's graphics interrupt callback with `source = 1`, synchronously on the calling thread, posing as a DPC (`:652–663`; `kernel/kernel_state.cc:1407–1447`) [V].
- **Ring read pointer write-back:** once per batch, not per block. The game's requested update frequency is ignored (`command_processor.cc:412–419`) [V/D].

## 4. Swap, vblank and pacing

- **`VdSwap`** (`kernel/xboxkrnl/xboxkrnl_video.cc:466–551`) [V]:
  - Writes a register packet pointing fetch constant 0 at the front buffer.
  - Then **`PM4_XE_SWAP` (0x64)**, an Xenia-only packet containing `'SWAP'`, the physical front-buffer address, width and height.
  - **There's no gamma ramp in it.** Gamma comes from the game writing the `DC_LUT_*` registers (`command_processor.cc:618–736`).
- **On `XE_SWAP`:** `IssueSwap` then `counter++`, and **no interrupt** (`pm4_command_processor_implement.h:666–690`) [V].
  - D3D12 presents as soon as possible with tearing allowed. The reason given: the host refresh rate may not be a multiple of 30 or 60 Hz (`ui/d3d12/d3d12_presenter.cc:1078–1089`) [D].
- **Vblank thread** (`gpu/graphics_system.cc:149–298`) [V]:
  - Ticks at `framerate_limit` (default 60 Hz with vsync on).
  - Each tick advances `D1MODE_V_COUNTER`, does `counter++` and calls the game's interrupt callback with `source = 0` on CPU 2.
  - On Windows it sleeps for 90% of the interval and spins the rest.
- **Pacing** [I]: a game's 30 or 60 fps cap (presentation interval 2 or 1) is **paced entirely by the game's own D3D counting these vblank interrupts**. Canary has no presentation-interval logic.
- **Edge separates them:** `guest_display_refresh_cap` (default on) gives a fixed 50/60 Hz vblank to the game, while `framerate_limit` only throttles host presents, "without affecting guest vblank timing" (`edge gpu/gpu_flags.cc:56–69`) [V]. That's the right model for unlocking frame rate.

## 5. Busy-waiting

- **No Canary setting for games that spin on GPU state.** There's no `enable_spin_wait` [V].
- **What does exist** [V]:
  - The command thread spins (with `MaybeYield`) for 500 iterations before waiting 2 ms on an event: "the overhead of waiting on our event is too high" (`command_processor.cc:377–404`).
  - `WAIT_REG_MEM` sleeps only with vsync on.
  - `KeDelayExecutionThread(0)` becomes `MaybeYield` or a 100 ns sleep depending on thread priority (`kernel/xthread.cc:971–1006`).
  - `delay_via_maybeyield` (default off) turns the CPU's `db16cyc` delay instruction into a yield.

## Edge differences
- Same occlusion query core, plus an `occlusion_query_log` setting.
- **Holds back fences and interrupts** until earlier memexport output has reached guest memory, with a 1 s limit (`edge gpu/command_processor.cc:764–850`). This only applies to memexport, and Gears 2 doesn't use memexport.

## What this means for your renderer [I]

1. **Occlusion queries:**
   - Bracket each ZPD segment with a D3D12 `OCCLUSION` query.
   - Write running totals in the 8-dword layout: split each count across the A and B lanes, and write the ZPass/StencilFail lanes last.
   - **Never block:** answer immediately with the last real result for that query, as fast mode does. UE3 already tolerates a frame or two of query latency, since its PC renderer works this way. **[I] - unverified; must be tested before relying on it (e.g. check chapter 2 for popping with answers delayed by one frame).**
   - When rendering above 720p or with different MSAA, **scale counts back to guest samples**, or UE3's visibility thresholds will shift.
   - Better still, once Stage 5 replaces D3D, answer at the game's `GetData` instead of through PM4.
2. **Fences:** signalling as soon as the packet is processed is safe **only if** you've already copied any guest memory the GPU will read (dynamic vertex buffers, constants, textures) by then. With upload-on-submit, which your Stage 1 hooks give you, signal immediately. That avoids any waiting.
3. **Swap:** keep a fixed 60 Hz game vblank (interrupt source 0 plus the counter) so the game's 30 fps pacing stays correct. Present on the host independently, as Edge does. Unlocking frame rate then becomes a game-side change, not a vblank hack.
4. **Busy-waiting:** with immediate fences and query answers, the game's spin loops exit on their first check. Any remaining `WAIT_REG_MEM` can return immediately in a native renderer. That frees the core Canary spends spinning.

Source: [Xenia Canary](https://github.com/xenia-canary/xenia-canary) `e6cd2e4bc`, with [Xenia Edge](https://github.com/has207/xenia-edge) `8fb2772` for comparison.