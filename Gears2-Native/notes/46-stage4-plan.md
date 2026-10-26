# 46 - Stage 4 plan: queries, sync, swap (this repo)

2026-10-21 (updated after job 113, upl-s4b; job 117 and our renderer's query answers, upl-s4c). Builds on roadmap `28` Stage 4, `47` (Canary/Edge reference rules, Brief 14), `35` (frame inventory),
`42`/`44` (our renderer), `00` D23-D31.

Labels:
- **[V-gen]**: read in the recompiled code. The bodies come from the older project's `Gears2-Recomp/generated/default`.
  It is the same game image: this repo's `generated/default/gow2_init.cpp` function table matches it apart from 2
  entries (48,591 vs 48,593 functions), and all 12 Stage 4 addresses below are function entries in this repo's table.
  This repo's own chunk holding them isn't in the upload (only `gow2_recomp.47.cpp`), so the bodies weren't read
  from this repo itself.
- **[V-sdk]**: read in the rexglue SDK source (`/tmp/claude-0/sdk-bd-wt`, upstream commit e6407f3 plus our patches;
  none of our patches touch queries).
- **[V-47]**: verified in Canary per notes/47.
- **[I]**: inferred.
- **[unverified]**: taken from the older project's notes and not checked here.

## 00. Status at close (2026-10-21 11:25, job 120) - Stage 4 queries CLOSED with open items

Decided with the user: job 120 was the last Stage 4 test; whatever it showed, we move on.

**Verified (job 120, ch02, 80 s each, `--gow2_renderer_queries=shadow` vs `=answer`):**
- Every query gets a real count from our renderer: incomplete 0-1 of ~64,000 per 300 frames (job 118's "all
  INCOMPLETE" was the write-nothing skip at shadow_renderer.cpp:947, fixed in job 119; job 119's own runs launched the
  menu because of the one-route PowerShell list, rerun as 120).
- About 83% of queried objects are hidden (count 0); before, the emulator answered "visible" (fake 1000) for all.
- **Draws per frame: 1,390 (shadow, game sees all-visible) -> ~1,050 (answer): ~24% fewer.** Pass criterion 1 met.
- Answers are on average 2.0 frames old; corrections 37-493 and "too late" 1,000-6,900 per 300 frames.

**Open items (not blocking):**
- **Popping not checked by eye** (the user could not watch job 120). "Estimate visibility wrong" = 48-1,052 per 300
  frames (~0.2-3.5 per frame): a one-frame wrong show/hide is possible. If seen: `--gow2_renderer_queries_min=1`.
  Easiest to judge once our image is on screen (notes/50).
- ch23 not rerun with the fix (job 118's ch23 runs had the skip bug).
- Why the SDK never takes its host-query path: section 0c, parked; patch 16 draft `patches/sdk-patch16-zpd-pairing-UNBUILT.diff`.
- Sync/swap (items 8-9): handover in notes/50 section 4; they belong with the takeover.

## 0. What the roadmap names, and what exists here

- **Not in this repo, the SDK or the SDK diff (upl-n095):**
  - job 195, `gow2_native_occlusion`, `NATIVEOCC`, `SetNativeOcclusionAnswer`;
  - `src/gow2_frame_pace.cpp` and its 60 fps fix (`8298CEC0` + PresentationInterval at dev+13596).
  These belong to the older project, so Stage 4 here starts from zero. Today the game paces itself (section 5).
- **Count correction.** notes/35 gives the occlusion pass in ch02 frame 1500 as draws 538-729 (192 draws). The
  "272 occlusion queries" figure is from **ch23** frame 1500 (job 071). The real per-frame query count comes from the
  new log (section 7).

## 0b. Job 113 result: the emulator gives no reference counts (upl-s4b)

`--gow2_query_log=300` on ch02 and ch23, stock SDK (patches 1-14).

**What the log shows:**
- **Queries per frame:** ch02 211-229 (max 663), ch23 263-281 (max 1,710).
- **Every begin report** sits at addr mod 64 = 32, which confirms the layout.
- **0-draw queries 0, pending 0, unread 0,** and the end marker is already set at every query's first draw.
- **Almost every answer is the SDK's fake count:** `end zpass 000003E8`, begin 0, mean 1000.0 samples.
  - Only one query per route wasn't fake: the first one.
  - Its begin report held stale memory, so the game got garbage: the ch23 frames 1-300 line shows a mean of
    1,591,105,351 over 2 queries.
- So today the game treats **everything as visible**: nothing is culled by occlusion.

**Cause [I, very likely; patch 15's log will confirm]:**
1. `d3d12_submit_on_primary_buffer_end` (default on) ends the D3D12 submission each time the guest's primary ring
   buffer ends (`OnPrimaryBufferEnd`).
2. `EndSubmission` has a "safety net" that ends any open host query and **forgets it**.
3. The query's end ZPD then finds no active query and calls `DisableHostOcclusionQueries()`. Host queries stay off
   for the session: every later ZPD takes the fake path (1000).
4. The only real count would have come from the first query, and the SDK never writes a begin report, so that one
   was end − stale memory.

**What this means (D23/D32):**
- The emulator is **not a usable reference** for query counts.
- The Stage 4 comparison becomes **"no popping, and fewer draws than all-visible"**, not "equal to the emulator":
  - our renderer gives real counts;
  - "everything visible" is today's behaviour, kept for comparison (D32: a deliberate, labelled deviation from the
    reference).
- For a D23 cross-check of rules, SDK patch 15 adds `--occlusion_query_no_fake` (default off: stock behaviour).
  - It keeps a query alive across submission ends (segments are summed), never disables host queries, zeroes
    the begin report, and writes 1 instead of 1000 when no real count can be had.
  - Its counts are a second opinion, not the truth.
  - Run it with `--gow2_query_log` to see real SDK counts. `--d3d12_submit_on_primary_buffer_end=false` is a
    cheaper single-switch test of the cause.

## 0c. Why the SDK never gives host counts (job 117; parked, unverified)

- [unverified] The SDK D3D12 ZPD handler accepts an end only at the begin's address (`command_processor.cpp`: the `sample_count_address != sample_count_addr` checks), but XDK 7645 puts the end report 32 bytes before the begin report (job 113: begin 1F5F5020 / end 1F5F5000). So stock turns host queries off at the first end (all FAKE), and no_fake answers every end with 1 (host 0) - both match job 117.
- Parked as `parked/sdk-patch16-zpd-pairing-UNBUILT.diff` (on top of 1-15): `--occlusion_query_end_any_address` (default off) pairs by order, plus a start-up status line and a once-only pairing warning. Not built, not part of upl-s4c's build.

## 1. The guest functions

The device is "dev". Fields used everywhere:
- dev+10908: the last fence issued;
- *(dev+10896): the fence value the GPU has written back;
- dev+10941 bit 1: a "GPU dead" flag.

| Address | Name | What it does | Label |
|---|---|---|---|
| `82978120` | Query Issue | BEGIN (flag 2): sets the report count to 1, or to the tile count dev+12748 when predicated tiling is on (dev+10940 bit 0x20). Type 9 (occlusion) then calls the ZPD writer with begin. END (flag 1): writes 0xFFFFFEED (big-endian) into the end report's ZPass_A/B, on the CPU if the last result was read (q+20 bit 0x80), otherwise through a GPU MEM_WRITE + WAIT_REG_MEM. Then it calls the ZPD writer with end, stores fence dev+10908 at q+24 and sets "issued" (q+20 bit 0x40). Type 8 (event) only stores a fence. Type 10 writes EVENT_WRITE 25 (screen extents). | V-gen |
| `82993080` | ZPD writer (new) | Writes RB_MODECONTROL=6, RB_SURFACE_INFO=0x20000 and RB_COLOR_INFO=0, then **RB_SAMPLE_COUNT_ADDR = report + 32·which** (which = 1 begin, 0 end), then `EVENT_WRITE_ZPD` with initiator 21 (ZPASS_DONE). When tiled, it does one SET_BIN_MASK/SELECT + SET_CONSTANT(RB_SAMPLE_COUNT_ADDR) per tile, then one ZPD. | V-gen |
| `829783A8` | Query GetData (the "query poll") | Type 9: not issued → D3DERR. Fence still pending → kicks the ring if needed and returns S_FALSE. Otherwise it adds up Σ over reports of (end.ZPass_A+B − begin.ZPass_A+B), with dcbf before each read; the GPU-dead flag gives 1. If **both** ZPass lanes of the last end report still hold the marker it returns S_FALSE; otherwise it sets q+20 bit 0x80 and returns S_OK. | V-gen |
| `8298C6D0` | IsFencePending | Kicks if the fence is the current one, then returns `(issued − fence) < (issued − completed)`. | V-gen |
| `8298C2A0` | BlockOnRingPosition | If the fence is pending: kick, then loop on `82977688` until it passes, then `829775C0`. | V-gen |
| `82977688` | GPU-wait step (the "spin") | 4×8 `db16cyc` delays. Then it checks the GPU-dead flag and whether *(dev+10896) moved. It returns 1 ("keep waiting") until no progress for ≥5000 ticks (r13+256 → +88), and then asks `829917C0` (a hang check, [I]). | V-gen structure, I meaning |
| `8298C650` | BlockUntilIdle | Writes WAIT_UNTIL (0x05C8) = 0x20000, then BlockOnRingPosition on the current fence, then spins while dev+11008 ≠ 0. | V-gen |
| `8298C1A0` | KickOff / MakeSpace | Unless dev+13232 is set or bit 0x80 of dev+10940 is set: takes the pending segment (`8298BC58`), writes it (`8298C0C8`) and kicks (`8298BF40`). It can also BlockOnRingPosition on fence−2. It returns the new write pointer. | V-gen calls, I meaning |
| `8298C5B8` | Interrupt callback | source 1: calls the callback at dev+10900→+16, then clears a bit under a spinlock. source 0 (vblank): calls `8298CBF8` when a global flag is set. Matches notes/47 §3-4 (INTERRUPT packet = 1, vblank = 0). | V-gen structure, V-47 sources |
| `8298CEC0` | swap/fence helper | Reads dev+13596 (PresentationInterval per notes/28) and calls KickOff 3 times. | V-gen read, meaning unverified |
| `8298D430` | Swap | Calls `8298CEC0`, BlockOnRingPosition, the GPU-wait step, KickOff ×7 and several kernel imports (VdSwap among them, [I]). | V-gen calls |
| `82989A10` | ring-space query | Reads dev+13232/13236/13516/14904-14920. | unverified meaning |
| `82977ED0` | sync/query release | Calls `8280F478`, `82990018`, `82AABD28`. | unverified meaning |

### Query object (type 9)

- +0 device, +4 type, +16 state, +20 flags (0x40 issued, 0x80 result read), +24 fence;
- +28 the report physical addresses [N], +148 N (V-gen).
- **Each report block is 64 bytes: the END report at +0 and the BEGIN report at +32** (V-gen, from both the writer and
  the poll). This is XDK 7645 D3D knowledge, so layer 0 takes it as a hint: `XenosQueryLogHints{-32}` from the app.
- **Untiled (D23 default) → N = 1** [I: follows from the code, not logged yet]. Tiled → one report block per tile.

## 2. How the game issues a query on Xenos, and what it reads back

1. Issue(BEGIN): RB_SAMPLE_COUNT_ADDR = begin report, ZPD.
2. The query's draws: in ch02 these are depth-test-only draws, no colour (notes/35).
3. Issue(END): the marker goes into the end report, then RB_SAMPLE_COUNT_ADDR = end report, ZPD.
4. The CPU polls GetData until the fence has passed **and** the marker is gone.
5. Result = end.ZPass(A+B) − begin.ZPass(A+B), 32-bit wrap.

The hardware writes a running counter snapshot at each ZPD (V-47 §1); D3D subtracts.

## 3. How the SDK answers today [V-sdk]

`D3D12CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD` (`src/graphics/d3d12/command_processor.cpp:154-221`;
upstream, not one of our patches). It is on by default: `occlusion_query_enable=true`.
- **End or begin** is decided by the marker in ZPass_A/B or ZFail_A/B of the report at RB_SAMPLE_COUNT_ADDR.
- **Begin:** a D3D12 `BeginQuery` on a 4096-slot heap. **The begin report is not written** (only the fake/base path
  zeroes it).
- **End:** `EndQuery` + `ResolveQueryData`. Then it ends the submission and **blocks the GPU thread on the fence**
  (`CheckSubmissionFence` → `WaitForSingleObject`, `:3241-3279`). Then it writes Total_A = ZPass_A = n (divided by
  the resolution scale) and the other lanes 0 (`:5060-5110`).
  - So the SDK is Canary's **strict** mode, minus the time limit: one full submit + CPU wait per query. That is about
    192-272 GPU round trips per frame on the GPU worker thread.
- **Fallbacks:**
  - Either of these turns host queries off **for the rest of the session**: a begin while another query is open, or
    an end at a different address.
  - Any failure writes `query_occlusion_fake_sample_count` (default **1000**).
  - So a run can quietly change from real counts to "everything visible". The new log counts answers equal to 1000
    as `fake?`.
- **Begin report never written:** if it holds non-zero memory, the game gets end − garbage. The log counts
  `begin reports not 0`.

## 4. "Our renderer answers queries"

### Shadow mode (next; the emulator still answers)

- In OnDraw the renderer runs the same `QueryTracker` (`xenos/renderer/queries.h`) as the log.
  - `opened` → `BeginQuery` on our own D3D12 occlusion heap, around our copies of that query's draws.
  - `closed` → `EndQuery` + `ResolveQueryData` into a readback buffer. We never wait.
- After our frame fence: fill `our_samples` and scale it to guest samples (÷ resolution scale², and the MSAA factor
  when we draw at a different sample count, V-47 §"what this means" 1). Then `Compare()` each record:
  `same` / `close` (≤5%) / `count-differs` / `VISIBILITY-DIFFERS` / `no-ours` / `no-emulator`.
- **Logged:** a per-frame summary, plus one line per visibility disagreement (address, draws, both counts).
- **D3D12 rule:** Begin and End must be in the same command list. If our recorder closes a list inside a query,
  split the query into segments and add them up (Canary's pool does this, V-47).
- **Backend:** gpu.h needs a small addition, owned by the renderer helper: `CreateOcclusionHeap`,
  `BeginOcclusion(slot)`, `EndOcclusion(slot)`, `ResolveOcclusion(first, count)`, plus a mapped readback.
  `OcclusionAnswerSource` in queries.h is the shape the renderer exposes upward.

### Implemented in upl-s4c: our counts, and our answers in shadow mode

The switch is `--gow2_renderer_queries=off|shadow|answer`, default off. It needs SDK patch 15 and the hub's OnZpd
forwarder.

- **Backend (gpu.h, d3d12_backend.cpp):**
  - Begin/End/GetResult/Release on a 16,384-slot `D3D12_QUERY_TYPE_OCCLUSION` heap, resolved into a mapped readback
    buffer.
  - One query open at a time. A Submit inside a query ends that part and continues it in the next command list (up
    to 16 parts, summed).
  - Never waits: no free slot → refused.
- **Shadow renderer, at the ZPDs:**
  - **Begin ZPD:** BeginOcclusionQuery before that query's draws. In `answer` mode the begin report is also zeroed;
    the SDK leaves stale memory there.
  - **End ZPD:** EndOcclusionQuery. In `answer` mode the end report gets our immediate answer straight away, in the
    SDK layout (Total_A = ZPass_A = n, rest 0, ZPass/StencilFail lanes last).
- **The immediate answer:**
  - the last count we had for that report address, or 1 if none (Canary fast);
  - `--gow2_renderer_queries_min=1` never answers 0 before our count for that query has landed.
- **When our count lands** (polled at the swap, after our frame fence):
  - it becomes that address's next answer;
  - the report is corrected if it still holds our write (`--gow2_renderer_queries_correct`, on).
- **Incomplete queries:** if any draw inside the query was not drawn by us (pending shader/pipeline), the count is
  neither learned nor written.
- **Counts are guest samples:** we draw at guest resolution and sample count (scale 1; `AnswerCache::ToGuestSamples`
  is ready for scaling).
- **Summary line** `gow2_renderer: queries (...)` every `--gow2_renderer_log_every` frames:
  - ends/frame, ours/incomplete/refused/unended;
  - answers from last count vs the fallback 1;
  - our counts (0 share, mean, latency in frames), corrected, too late, how often the estimate's visibility was wrong;
  - SDK answers, **draws/frame** (the "fewer draws" criterion), backend segments.
- **Per-query lines:** `--gow2_renderer_queries_log=N`.
- **`shadow` mode** writes nothing: it measures what the answers would be, and the draws/frame there are the
  all-visible baseline.
- **Open:** whether one frame of answer latency causes popping (the [I] in notes/47) is tested by this very job.
  `answer` with last-count estimates *is* the delayed-answer case.

### Takeover (after Stage 3; the emulator no longer draws)

There are two ways to answer:
- **(a) At the ZPD (PM4 level)** (first choice, works before Stage 5). Write our answer into the end report with
  `SampleCounts::WriteAnswer` (ZPass lanes last). We never block. Until our real count arrives, the answer is the
  **last real result for that report address, with 0 treated as 1** (Canary "fast", V-47 §2). It is corrected when
  our readback lands.
  - **Open question / gate: does Gears 2 tolerate a frame of query latency?** The claim that UE3 tolerates a frame or
    two of query latency is an **[I] (unverified, notes/47 item 1)**.
  - Test before (a) is final: chapter 2 (plus the ch23 route) with answers deliberately delayed by one frame, checked
    for popping by eye and against the shadow log's visibility. Needs a ZPD hook (work item 2).
  - If it fails: strict-with-time-limit (wait for our query up to ~2 ms, floor 1), as Canary's strict mode does.
- **(b) At GetData (D3D level, after Stage 5):** hook `829783A8` and return our count, or S_FALSE until it's ready.
  Hook `82978120` for begin/end. No report memory is involved. This is the roadmap's "answers the poll immediately".

## 5. Fences, sync and swap in takeover

- **Fences** (V-47 §3): signal when the packet is processed, i.e. write the fence value the game reads back at
  *(dev+10896). That is safe because our renderer copies guest memory at submit (Stage 1 hooks, D24).
  IsFencePending, BlockOnRingPosition and BlockUntilIdle then pass on their first check.
- **WAIT_REG_MEM:** return immediately once our writes are done.
- **Swap:** VdSwap → `PM4_XE_SWAP` (V-47 §4). We present our frame.
- **Frame pacing:**
  - **Keep the SDK's fixed 60 Hz vblank** (interrupt source 0 + the counter): the game paces 30 fps itself by
    counting vblanks (V-47 §4).
  - Our present runs independently (Edge model).
  - There is no `gow2_frame_pace` here to move. A future frame-rate unlock is a game-side change (Stage 7), not a
    vblank change.

## 6. Expected CPU saving [I, to be measured]

1. **SDK GPU thread:** the per-query submit + wait (section 3) disappears in takeover, since we never block. Shadow
   mode keeps it, because the emulator still answers.
2. **Game threads:**
   - the GetData polls (the roadmap's 16,379 calls/frame in the menu, older project, [unverified] here);
   - `82977688` (0.6-1.0 M calls/frame in the older project, [unverified]).
   Both wait for the emulated GPU to catch up. With immediate fences and answers they should exit at the first check:
   that is the "freed core".
3. **Measure with:**
   - `--gow2_frame_times`;
   - a call counter on `82977688` / `829783A8` (layer-3 manifest midasm hook, work item 6);
   - the renderer's split line.

## 7. The tool: `--gow2_query_log=N` (upl-s4a; exact ZPDs since upl-s4b)

**upl-s4b:**
- With SDK patch 15 the log gets every ZPD exactly, through `DrawObserver::OnZpd`: address, begin/end, and the SDK's
  answer kind (host / FAKE / no-real / none, plus the host segment count). The observer hub forwards it.
- Per-query lines add `sdk <kind> <samples> seg <n>` and `UNENDED` (a begin came before the end).
- The summary adds the counts by kind, split and unended queries, and the cumulative begin/end/unmatched ZPDs.
- The register-following description below is now the tracker's fallback ("inferred") mode.


- **Code:** `src/layer0_xenos/query_log.cpp`, observer-hub tool, layer 0. It uses `QueryTracker` from `queries.h`.
- **Every Nth frame it logs one line per query:**
  - begin/end report address (the ZPD targets);
  - its draws [first..last];
  - whether the end report already held the marker at the query's first draw;
  - the emulator's final end-report lanes (ZPass/Total/ZFail/StencilFail A/B) and the begin report;
  - the answer the game computes.
- **Summary line for that frame:**
  - answers 0 / >0 / pending / unread / `fake?`;
  - 0-draw queries;
  - begin reports that aren't zero;
  - begin-address alignment mod 64 (expect 32);
  - VIZ events;
  - draws after a ZPD.
- **Totals over every N frames.**
- **How it sees ZPDs without an SDK hook:**
  - the register holds the begin report during the query's draws;
  - any address change at a draw means the previous query has closed. The SDK writes the end synchronously, so the
    answer is final by then.
- **Limits:**
  - a query with no draws between its begin and end can show up as a phantom 0-draw "query" on its end address;
  - two empty queries on the same report are missed.
  The summary counts make both visible. The SDK hook in work item 2 removes the guesswork.
- **Suggested run:** ch02 frame ~1500 route, `--gow2_query_log=300` (full detail every 300 frames, totals every 300).
  Add `--gow2_query_log_max_lines=400`.

## 8. Pass criteria

1. **Reference log (ch02 and ch23):**
   - stock SDK: job 113 failed `fake?` = 0 (section 0b), as expected now. With `--occlusion_query_no_fake`: sdk FAKE 0,
     no-real ≈ 0, split > 0 if the cause is right;
   - pending = 0;
   - begin alignment = 32 for all;
   - 0-draw queries ≈ 0;
   - query count per frame stable on a calm route.
2. **Shadow:**
   - against the stock SDK there is no reference (all fake). Compare() returns `no-emulator` for fake answers.
   - against `--occlusion_query_no_fake` host counts: 0 `VISIBILITY-DIFFERS` on steady frames of the 3 scenes, or each
     one explained; median count difference ≤5% (at 1x; scaled at 2x). This is a second opinion, not the truth (D23).
   - our own counts are plausible: a fraction of queries come back 0 behind walls (ch02 cover). That fraction is
     logged per frame. Check that the culled objects really are hidden (frame shots).
3. **Latency gate (§4a):** no visible popping with one-frame-late answers. If it fails, takeover uses strict with a
   time limit.
4. **Takeover:**
   - no popping on the 3 scenes (user's eye + video). This is now the main test, since the emulator never culls;
   - fewer draws than today's all-visible frames (`--gow2_draw_stats`), with the same image;
   - stable 30 fps pacing (`--gow2_frame_times` p99 within 1 ms of 33.3);
   - the wait-spin counters drop to near the first-check minimum.

## 9. Risks

- **The SDK falls back to fake 1000 almost at once** (section 0b, job 113). There is no emulator reference; patch 15's
  switch gives a second opinion.
- **The begin report is never written by the SDK:** stale contents skew the emulator's answers. Logged.
- **Tiling on (`--gow2_untiled=false`):** N reports per query, a SET_CONSTANT path for RB_SAMPLE_COUNT_ADDR and
  per-tile ZPDs. The tracker then sees only the last tile's address; the SDK disables host queries on the address
  mismatch. Supported: untiled only (D23/D26).
- **Command-list splits inside a query** (§4).
- **Resolution/MSAA scaling of our counts:** UE3's visibility thresholds are in guest samples.
- **The latency question (§4a), unverified.**
- **Fences signalled before our GPU has used guest memory:** safe only while we copy at submit.
- **Function meanings marked I/unverified** (82989A10, 82977ED0, KickOff internals) could matter for takeover sync.

## 10. Ordered work list

| # | Item | Who | Estimate |
|---|---|---|---|
| 1 | Build + run `--gow2_query_log` on ch02 and ch23 (one job), read the reference answers, check section 8.1 | user run + us | 0.5 d |
| 2 | **Done (upl-s4b): SDK patch 15**, `DrawObserver::OnZpd(regs, ObservedZpd)` after the report is written, plus `--occlusion_query_no_fake`. The latency test and takeover (a) still need a write hook (the observer can write the report in OnZpd, but the guest may read the SDK's value first). Add `uint64_t* samples` before the write when item 5 starts | us | 0.5 d (done) + 0.25 d |
| 3 | Backend occlusion-query API (gpu.h, d3d12_backend) | renderer helper | 0.5-1 d |
| 4 | Shadow: QueryTracker in shadow_renderer, bracket draws, readback after the frame fence, scale, Compare, log | renderer helper (we supply queries.h) | 1-1.5 d |
| 5 | Latency test: delay the emulator's answers by one frame through patch 7, ch02 + ch23, popping check | us + user | 0.5 d + run |
| 6 | Call counters on `82977688` / `829783A8` / `8298C2A0` (layer-3 midasm hooks), baseline numbers | us | 0.5 d |
| 7 | Takeover queries (a) behind `--gow2_renderer_queries=takeover` | us | 1 d |
| 8 | Takeover fences/sync: immediate fence write-back, WAIT_REG_MEM, check the spins exit | us | 1 d (after Stage 3) |
| 9 | Takeover swap/present with the SDK vblank kept; pacing check | us | 1-1.5 d (after Stage 3) |

Total ≈ 6.5-8 d plus runs; the roadmap had 3-5 d. Items 1-6 can go before Stage 3 (the roadmap's "queries could go
before Stage 3"); items 7-9 wait for Stage 3.
