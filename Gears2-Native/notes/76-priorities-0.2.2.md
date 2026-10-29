# 76 - Priorities after 0.2.2 (28 Oct 2026)

Decided by Shagg, 28 Oct 2026. [V] = measured / read in code, [S] = from a source, [G] = guess.

## 1. TOP PRIORITY: DirectX 12 running properly on SteamOS / Steam Deck - before more Vulkan work

**Why D3D12 first:**
- D3D12 is the renderer that gets every fix: the AMD depth read-back (0.2.2), upscalers, frame generation, frame pacing.
- Under Proton, D3D12 is translated to Vulkan by vkd3d-proton anyway, so Deck players would get the same picture as Windows players.
- Our own Vulkan backend (notes/68, 71, 75) lags on everything:
  - Gears 3 darker, with black patches
  - Judgment's scrambled text (#27)
  - no upscalers or frame generation
  - no Xenos MSAA sample positions (patch 0089)
- Keeping two renderers means making every fix twice. Vulkan stays for Mac later.

**Known:**
- [V, #15 logs] On a Steam Deck the D3D12 build exits with code 3 right after the renderer starts.
- [S, #27] One SteamOS desktop with an RX 9070 XT reported that D3D12 did start. So it depends on the setup (vkd3d-proton version, GPU, driver).
- [V] Since 0.2.1 the launcher forces Vulkan under Proton / Wine (gears_settings.cpp: `vk = Api == 1 || env.wine`).

**Suspects (unverified):**
- [G] The Agility SDK files shipped beside each game: `D3D12\D3D12Core.dll` plus the exe's `D3D12SDKVersion` / `D3D12SDKPath` exports. These may not be handled by vkd3d-proton.
- [G] A feature the Deck's driver or vkd3d-proton lacks: programmable sample positions, Shader Model 6.6, typed UAV loads, a resource-binding or heap tier, ROVs.
- [G] The DXGI swap chain / presenter path (flip model, frame latency waitable, tearing).

**Plan:**
1. Add a launcher override so testers can force D3D12 under Proton, for example `extra-args` or a setting that wins over the Wine check.
2. Get one Deck log with D3D12 forced. The log names the failing step; exit code 3 = the renderer's device or presenter set-up.
3. Fix that step, ship it as a test build, and get Deck testers to confirm.
4. Switch the Proton default back to D3D12. Keep Vulkan as a choice.

## 2. Next renderer work

- **#30, light glows through walls (occlusion queries).**
  - Our counting (`--gow2_renderer_queries=answer`) answers with the report's last count. The real count lands about 2 frames later.
  - [V, Gears 3 run 20261028-110333] About 20% of answers had the visibility wrong, and 4,000-5,000 corrections per 300 frames arrived too late, so everything flickered. Counting stays off in all games.
  - Fix: give the game a "not ready" answer it waits on (it has a query wait loop: gow3_patch_query_poll_yield), or have the real count in time.
  - Also keep our own full-screen passes out of an open query [S: VinceIP/fci-recomp PR #36].
- **#25, RAAM's Shadow "coming soon" with many DLC packages.**
  - [V] The players' package `9BA6A0FD894515150B74027D026393740FAFD6654D` is identical to a working one. With only Season Pass + Campaign installed, RAAM's Shadow appears.
  - The runtime enumerator returns every package [V, read in code], so the cut-off is in how the game uses the list, or another package takes the campaign's slot.
  - Next: a `--log_level=debug` log with all DLC installed.
- **RAAM's Shadow bank vault:** reloading a checkpoint fails; the chapter has to be restarted. No log detail yet.
- **#27 Judgment text on SteamOS / Vulkan.** Partly superseded by item 1.
- **#23 D3D12 judder in Gears 2.**

## Done in 0.2.2 (for reference)

- AMD depth read-back (`XeUnorm24`, the transfer self-test).
- Graphics card choice (SDK patch 44, #29).
- No Tracy / debug runtime in the release; leftovers are deleted at start-up.
- One-click update with auto restart.
- Gears 2 TU6 camera shake (title update loose inis).
- Back + Start combo guard (patch 43); keystroke block (patch 41).
- The fps bar no longer forces monitor-rate repaints (patch 42).
- Problem report wording for generic Windows user names.
- Xenos MSAA positions tested in Gears 3: standard looked better, so they stay off.
