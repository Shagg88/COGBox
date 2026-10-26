# 32 - rexglue GPU hooks and UE3 test-route options (2026-10-20)

Research from the second chat (Briefs 4 and 5), pasted back here. Labels as in the text: [V] verified in source,
[D] documented by Epic, [C] community-reported, [G] guess.

**Checked here against the loaded game image (strings in default.xex's image, job 014 dump):** the wide strings
`nostartupmovies`, `nomovie`, `BENCHMARK`, `FIXEDSEED`, `UNATTENDED`, `FORCELOGFLUSH`, `chapter` and `loadcheckpoint` are all
present, so this build very likely parses them (not yet proven by a run). `geargame_p` is not a string in the exe; the map
is the package `GearGame_P.xxx` on the disc.

**Consequences for the plan:**
- Our renderer can't be a second GPU plugin: rexglue has one graphics slot. The draw observer goes in as a small SDK patch
  just before `IssueDraw` (shared `command_processor.cpp:1381`) and at `IssueSwap` (line 961) - patch 2 in
  `patches/rexglue-sdk.diff`, Stage 0. Our picture can be shown next to the emulator's through a presenter `UIDrawer`
  without any patch; replacing the emulator's picture needs a patch around `RefreshGuestOutput` (Stage 3).
- Chapter starts for test routes: `--gow2_launch="GearGame_P?loadcheckpoint=0?chapter=<index> -nostartupmovies"`
  (index = chapter - 1), table below. Source: the santiquiroz Gears 2 port's docs (BSD-3), cited, not copied code.

---

Both briefs are done. Brief 4 was read at the requested tag, `nightly-20261002-bd833a2a`; for Brief 5, the most useful public source is the santiquiroz Gears 2 port, which documents chapter travel in the game's own script.

**Bottom lines:**
- **Brief 4:** rexglue has exactly one graphics slot and no draw, resolve or present hooks upstream. The cleanest place to see every draw with complete register state is just before `IssueDraw` in the shared command processor, and that needs a small SDK patch.
- **Brief 5:** Gears 2's chapters are mostly sections inside shared `SP_*_P` persistent maps, started by travelling to `geargame_p?loadcheckpoint=0?chapter=<index>`.

Labels: **[V]** verified in source, **[D]** documented by Epic, **[C]** community-reported, **[G]** my guess.

---

## Brief 4: rexglue GPU plugin architecture

### The plugin interface

**What a plugin must export** (`include/rex/system/gpu_plugin.h`):
- Two `extern "C"` functions: `uint32_t rex_gpu_abi_version()` and `IGraphicsSystem* rex_gpu_create(uint32_t abi, const GpuCreateInfo*)` (lines 42–47). **[V]**
- `GpuCreateInfo` holds only `struct_size` and `backend` (`"d3d12"`, `"vulkan"` or `"any"`) (lines 37–40), and the ABI version is 1 (line 32). **[V]**

**How the loader works** (`src/system/gpu_plugin_loader.cpp`): **[V]**
1. `PluginFileName` (31–46) builds `rexgpu-<name>[d|rd].dll` (or `.so`/`.dylib`), using the same per-config suffix as the SDK.
2. `LoadGpuPlugin` (57–103) loads that file from the executable's folder.
3. It checks the two exports and the ABI version, then calls the create function.
4. Libraries are kept loaded until the process exits (48–53, 100).

**The xenos plugin** (`src/graphics/plugin_main.cpp`) implements both exports (24–53). It returns `D3D12GraphicsSystem` for `any`/`d3d12` (so D3D12 is preferred), or `VulkanGraphicsSystem`. **[V]**

**How `--gpu_plugin=xenos` gets there** (`src/ui/rex_app.cpp`):
- The `gpu_plugin` option is read at startup only; empty means no GPU emulation (51–54). **[V]**
- In `SetupPresentation` (306–323), `OnPreSetup(config_)` runs first. A plugin is loaded only if `config_.graphics` is still empty afterwards. **[V]**
- The result moves into the runtime (`src/system/runtime.cpp` 183–185). **[V]**
- The kernel's `Vd*` video exports, including ring-buffer setup and interrupts, go to that single system (`interfaces/graphics.h`; warning at `xboxkrnl_video.cpp:60`). **[V]**

**Can two plugins coexist? No:**

- There's one `IGraphicsSystem` slot, and only it receives the guest ring buffer. **[V]**
- More than one library can be loaded, but only one gets the GPU. **[I]**
- Your app *can* supply its own system through `OnPreSetup`, for example a wrapper that owns the xenos system. **[V]**
- `IGraphicsSystem` (`interfaces/graphics.h`) exposes only setup, presenter, ring-buffer, interrupt and shader-storage calls. A wrapper therefore can't see draws through it. **[V]**

### How the command processor runs draws

**Shared code** (`src/graphics/command_processor.cpp`; class at `include/rex/graphics/command_processor.h:76`): **[V]**
- `WorkerThreadMain` (line 200) runs the GPU thread. It calls `ExecutePrimaryBuffer` (615) and `ExecuteIndirectBuffer` (636), which feed `ExecutePacket` (665); the packet switch is at 767–813.
- **Draws:** `DRAW_INDX` (1406) and `DRAW_INDX_2` (1423) share one path, which ends with the virtual `IssueDraw` call at **1381**.
- **Constants loaded from memory:** `LOAD_ALU_CONSTANT` (1472) writes them into `register_file_`, and `WriteRegister` (header 145) is virtual.
- **Swap:** `XE_SWAP`, Xenia's own VdSwap packet (929–961), calls `IssueSwap` at 961.

**D3D12 backend** (`src/graphics/d3d12/command_processor.cpp`): **[V]**
- `IssueDraw` (2271) routes `edram_mode == kCopy` to `IssueCopy` (2284 → 2873). Resolves therefore also arrive through `IssueDraw`.
- `IssueSwap` (1894) passes the frontbuffer to `presenter->RefreshGuestOutput` (1988).

**Cleanest observation point: just before line 1381.**
- At that moment `register_file_` holds the complete state: constants, fetch constants, render state and memory-loaded values. The index-buffer info is decoded, and it's shared by the D3D12 and Vulkan backends. **[V]**
- Add a small observer to `CommandProcessor` with `OnDraw(regs, prim, count, index_buffer_info)` and `OnSwap(frontbuffer)`. Fire it there and at line 961, and register it through the graphics system. **[I, design]**
- Alternative: subclass `D3D12CommandProcessor`, since `IssueDraw` is virtual and `GraphicsSystem::CreateCommandProcessor` is virtual (`graphics_system.h:100`). That needs plugin internals to be linkable from your code, which I haven't checked. **[G]**

### How the image reaches the window

- **Presenter API** (`include/rex/ui/presenter.h:183`): `RefreshGuestOutput(w, h, aspect…, refresher)` (358) gives the refresher a backend context; on D3D12 that's `D3D12GuestOutputRefreshContext`. Painting happens in `PaintAndPresent`. **[V]**
- **Drawing next to the game's image:** register a `UIDrawer` with `AddUIDrawerFromUIThread(drawer, z_order)` (371). On D3D12 its `Draw()` receives a `D3D12UIDrawContext` that has the `command_list()` for the swap-chain pass (`ui/d3d12/d3d12_presenter.h` 26–36). The ImGui overlay already uses this (`imgui_drawer.cpp:131`). **[V]**
- **Showing your image instead of the game's:** call `RefreshGuestOutput` yourself with your own refresher. `IssueSwap` also calls it every swap, though, so the emulator's call has to be suppressed or replaced. That needs an SDK patch. **[I]**
- Your existing `NativeGuestOutput…` / `SetNativeGuestOutputScale` API doesn't exist upstream; it's your patch. **[V by grep]**

### Existing hooks upstream

There are no draw, resolve or present observers or callbacks. A search for observer, listener, callback and hook in `src/graphics` and `include/rex/graphics` found none. **[V]**

What does exist:
- presenter UI drawers;
- the guest-output paint config;
- `IGraphicsSystem`'s optional virtual functions;
- internal virtual functions on command-processor subclasses (`OnPrimaryBufferEnd`, `OnGamma…`).

---

## Brief 5: UE3 command-line options for test routes

**Caveat first:** I couldn't confirm that the 2008 Xbox retail build parses any of these switches. **[G]** Two cheap checks:
- **Search the XEX for the strings.** UE3 checks them with `ParseParam(appCmdLine(), TEXT("…"))`, so the wide strings `nostartupmovies`, `benchmark`, `fixedseed` and so on would appear in the binary if they're parsed.
- **Find how the runtime passes a command line to the guest.** The public port avoids the question by sending console commands through a command file.

| Need | Option | Status |
|---|---|---|
| Skip intro movies | `-nostartupmovies` | **[C]** a UE3 game (Red Orchestra 2) |
| | `-nomoviestartup` | **[C]** UT3, with mixed reports |
| | Remove the `StartupMovies=` lines under `[FullScreenMovie]` in the engine ini | **[C]** UT3. For Gears 2 that means editing the coalesced engine ini, using your coalesced edit tool **[G]** |
| Difficulty | URL `?Difficulty=` | **[D]** but as a UTGame bot-skill option, so I wouldn't rely on it for Gears 2 **[G]** |
| | Better check: look for `GetIntOption` keys near `"chapter"` in `GearGameSP_Base.InitGame` | **[G]** |
| Chapter / checkpoint | `servertravel geargame_p?loadcheckpoint=0?chapter=<index>`, where index = chapter − 1 | **[C, verified in script by the santiquiroz port]**: read in `GearGameSP_Base.InitGame` as `GetIntOption(Options,"chapter",-1)` |
| | `?loadcheckpoint=1` resumes the last checkpoint | **[C]** same source |
| | Exec commands `SaveCheckpoint`, `LoadCheckpoint`, `RestartChapter`, `UnlockAllChapters` | **[C]** same source |
| Co-op / split-screen | Generic UE3 `?listen`, `-LANPLAY`, `-NOLIVE` | **[D]** |
| | Gears 2 co-op | **[C]** the port uses System Link and multiple instances |
| | Starting split-screen from the command line | **[G]** a second signed-in controller probably joins through the UI |
| Attract mode / idle timer | No documented switch found. `-UNATTENDED` disables user-feedback prompts, not necessarily attract mode | **[D]** for `-UNATTENDED`; the rest **[G]** |
| Benchmark / fixed timestep | `-BENCHMARK` (fixed step, no skipped frames), `-FPS=` (frame rate for benchmarking), `-SECONDS=` (maximum tick time), `-FIXEDSEED` (random seed 0), `-DUMPMOVIE` | **[D]** |
| Logging and exec | `-EXEC=<file>`, `-LOG` / `LOG=<file>`, `-FORCELOGFLUSH` | **[D]** |
| | Retail builds may compile logging out | **[G]** |
| | `-ini:File:Section.Key=Value` only exists in builds after May 2012 | **[D]**, so it doesn't apply to Gears 2 |

### Gears 2 chapters vs maps

Chapters are mostly **sections inside shared persistent maps**. Several chapters share one `SP_*_P` map, and you enter through the `geargame_p` transition map with `?chapter=`. **[C]**

From the port's `docs/TEST_TOOLING.md` (citing `GearTypes.ChapterLevelNames`):

| Chapter index | Human chapter | Persistent map |
|---|---|---|
| 0 | 1 | `sp_hospitalintro_p` |
| 1 | 2 | `sp_hospital_p` |
| 2 | 3 | `sp_assaultintro_p` |
| 3 | 4 | `sp_assault_p` |
| 4–5 | 5–6 | `sp_landown_p` |
| 6–8 | 7–9 | `sp_intervention_p` |
| 9–10 | 10–11 | `sp_rescue_p` |
| 11 | 12 | `sp_riftworm_p` |
| 12–14 | 13–15 | `sp_outpost_p` |
| 15 | 16 | `sp_mountkadar_p` |
| 16–17 | 17–18 | `sp_leviathan_p` |
| 18–19 | 19–20 | `sp_maria_p` |
| 20–21 | 21–22 | `sp_nexis_p` |
| 22–23 | 23–24 | `sp_palace_p` |
| 24 | 25 | `sp_escape_p` |
| 25 | 26 | `SP_JacintoIntro_P` |
| 26–27 | 27–28 | `sp_sinkhole_p` |
| 28 | 29 | `sp_closure_p` |

**For your test routes:** use `?chapter=<index>` to start a chapter, plus `SaveCheckpoint`/`LoadCheckpoint` for fixed mid-chapter starting points. Combined with `-fixedseed` and `-benchmark`, if the build honours them, that gives repeatable runs. **[G]** for the combination.

Sources:
- [rexglue/rexglue-sdk (tag nightly-20261002-bd833a2a)](https://github.com/rexglue/rexglue-sdk)
- [UDK: Command-line arguments](https://docs.unrealengine.com/udk/Three/CommandLineArguments.html)
- [santiquiroz/gears-of-war-2-pc-port, docs/TEST_TOOLING.md](https://github.com/santiquiroz/gears-of-war-2-pc-port)
- [HardForum: UT3 mini tweak guide](https://hardforum.com/threads/ut3-mini-tweak-guide.1245728)
- [Steam: Rising Storm/Red Orchestra 2 (-nostartupmovies)](https://steamcommunity.com/app/35450/discussions/0/846959179209171028)
