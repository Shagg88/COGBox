# 21 - The 512 MB cap, the texture pool, and the Jacinto multipatch ideas (2026-10-23 evening)

Labels: [V] verified, [S] source read, [G] guess.

## How guest memory is laid out (SDK src/system/xmemory.cpp, Memory::Initialize) [S]

| Guest range | What | Size | Counts against 512 MB? |
|---|---|---|---|
| 0x00000000-0x3FFFFFFF | virtual, 4 KB pages (NtAllocateVirtualMemory) | 1 GB | **no** - its own host memory |
| 0x40000000-0x7EFFFFFF | virtual, 64 KB pages | ~1 GB | **no** |
| 0x80000000-0x9FFFFFFF | the XEX image | 512 MB | no |
| 0xA0000000 / 0xC0000000 / 0xE0000000 | three windows (64 KB / 16 MB / 4 KB pages) onto ONE physical heap | 512 MB | this IS the 512 MB |

- The physical heap (`heaps_.physical`, 0x20000000 bytes) is what the GPU sees: textures, the texture pool, vertex and index
  buffers, resolve targets. GPU addresses are physical (the MMIO handler covers physical_membase..+0x1FFFFFFF).
- On the console every virtual page also used real RAM. In the SDK it doesn't: virtual allocations are free host memory.
  MmQueryStatistics reports 512 MB total minus the *physical* heaps' use only.
- The 360 devkits' 1 GB: the extra RAM was "debug memory" (the DebugMemory flag of NtAllocateVirtualMemory), meant for
  tools. Canary ignores the flag; retail games don't use it [S, xenia-canary xboxkrnl_memory.cc].

## Can the 512 MB be lifted?

- Physical (GPU) memory beyond 512 MB: no practical way [G, strong]. The three windows fill 0xA0000000-0xFFFFFFFF of
  the 32-bit address space exactly, so there is no room for a bigger window. The game's D3D library, the GPU's memory
  addressing, the resolve and texture caches all assume 512 MB. It would be a redesign of the shared core, and every
  game's assumptions would break with it.
- Not needed for most things: the game's CPU-side data is already outside the cap if UE3 takes it with
  NtAllocateVirtualMemory, and all our own work (renderer copies on the GPU, shader caches, prebuilds) is host memory.
- So the real limit on the texture pool is how much of the physical heap the game leaves free. Measure, don't guess:
  job 113 adds `--gow3_memory_log_seconds=10` (physical used/free, lowest free, virtual 4K/64K use).

## Texture pool (job 112) [V]

- Stock Xbox360-GearEngine.ini [TextureStreaming] PoolSize=145, MemoryMargin=10, StopIncreasingLimit=12,
  StopStreamingLimit=8 (from Coalesced_INT.bin). `--gow3_texture_pool_mb` (default 200) hooks 0x829678C0, after
  `slwi r11,r26,20` in sub_829677A8. Allowed range 1-400.
- Next: read the gauge from one big level, then size the pool as lowest free minus a margin.

## DryreL's "Jacinto Multi Patch Tool" (Nexus gearsofwar3/mods/27) - what carries over

It targets the PC port "Project Jacinto" (a leaked, unfinished PC build), so its boss/campaign/co-op fixes are for that
port's bugs and don't apply to the finished 360 game. Its engine-setting ideas, checked against our Xbox360 ini [V]:

| Idea | Xbox360 value now | Carries over? |
|---|---|---|
| MipFilter=Linear (trilinear) for TEXTUREGROUP_World/WorldNormalMap/Character | **MipFilter=point** | Yes. Removes the visible lines where texture detail steps down. ini edit, cheap |
| bEnableMipLevelFading=False (SSD profile) | not set (engine default) | Yes. Textures appear sharp at once instead of fading in |
| Streaming tuned for fast storage (MinTimeToGuaranteeMinMipCount etc.) | 2 / 12 | Maybe; after the pool measurement |
| MaxDrawDistanceScale 1.25, mesh LOD bias -1 | 1 / 0 | Tested in job 051 (scale 4). Could be offered as an option |
| Shadow resolution up | MaxShadowResolution=1120, WholeScene 1344 | Risky: shadow maps go through EDRAM (10 MB) and the 512 MB heap |
| MaxLODSize caps raised | 1024 | **No effect [V]**: the disc has no mips above the cap (see below) |
| HD Textures Pack (mods/28) | - | Not directly: PC-format packages. Would need texture replacement in our renderer |
| Roadie-run camera shake off | RoadieRunShakyCamDampeningFactor=0.6 (GearPawn.ini) | Yes, as an option |
| bUseTaskPerfTracking=False (startup DB connection) | TRUE, with a 10.1.20.20 DB | Check if our startup waits on it [G: likely compiled out on 360] |
| FPS limit / no smoothing | bSmoothFrameRate=TRUE, MaxSmoothedFrameRate 62 | Already have --gow3_max_fps |
| 16x aniso | MaxAnisotropy=4 | Already have --gow3_patch_aniso16 |

## Are there sharper textures on the disc than the game loads? (2026-10-23 22:2x) [V]

SP_OldShipyard_04_S, 248 Texture2D exports parsed (export table + tagged properties + the Mips array). 81 have a source
size of 2048 (World, WorldNormalMap, Vehicle, Character). For every one, mip 0 (2048) has bulk flags 0x21 (stored
elsewhere + unused) and size -1: the cooker stripped it. The top stored mip is 1024 (or 512 for specular, LODBias 1).
So raising MaxLODSize can't add detail. Sharper textures would need a texture pack, replaced in our renderer.

## The stats half of Gears 2's fix (RHIGetTextureMemoryStats -> FALSE): not found yet

Gears 2's 0x8280DDC8 (10 instructions: two pool counters out, return 1) has no look-alike in Gears 3: pattern search
gave 0 hits, and the function matcher found no match for its callers (0x823B7880, 0x823B80E8). The streaming manager's
config reader is 0x824991xx (StopIncreasingLimit -> this+0x1B0, StopStreamingLimit -> +0x1B4, MemoryMargin read at
0x8249932C). The budget function reads those fields. Parked: the bigger pool comes first, and this follows only if the
gauge or a play test shows the streamer cutting its budget.

## --gow3_tick_log (22:2x)

Engine tick rhythm every 600 ticks from the GetMaxTickRate hook: median / p95 / max, slow ticks, and their spacing. For
the "every half second" stutter (Shagg), and as data for Gears 2's frame pacing (idea #3).
