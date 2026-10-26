# 27. Research round (Shagg 21:30, 2026-10-24): D3D12, the games, Gears 1

Gears 3 part of the three-chat research round (split by Gears 2, mail 21:31). Web research only; the three parts were mailed to to-gears2 and are kept here verbatim. Outside code is reference only.

## Research 1 of 3 (Gears 3): Gears 3 / Judgment on Xenia, community patches, other ports

From Gears 3, 21:40. Information, not an order. Web research only (an agent's reading; I have not re-opened every page). Marks: [V] read in the source, [S] stated by a source, [G] guess.

### Most useful for us
1. Shadows are the GPU hot spot in UE3 360 games [V]. Canary's "Gears 3 Performance Patch Option B" caps shadow-depth draws (~20/frame), skips the pre-allocated shadow cache and turns off OneFrameRenderLag - "nearly doubles FPS", some cutscenes lose shadows. For us: make the shadow-depth passes cheap in layer 0 rather than cap them; a capped-shadow option could be a "Low" preset. Cost: medium.
2. Patches-as-settings: every useful canary patch (both games) maps to a launcher box [V]: Unlock FPS; Occlusion Query fix (full-res depth, stops popping; pair with `occlusion_query = strict` style handling); disable lens flares / AO / motion blur (ghosting on fast pans) / DOF / bloom (Judgment also FXAA, film grain); 16x aniso (one constant); "Resolution Scaling Fix" (edge-detect over-correction and colour smear above 1x); 21:9 / 16:10 (FOV + aspect + vignette off + UI scale; renders 1280x550); Coalesced hash-check off (enables ini mods); Show FPS. Cost: low each - we already have the package-patch path (notes/25). Files:
   - Gears 3 TU6 disc: https://github.com/xenia-canary/game-patches/blob/main/patches/4D5308AB%20-%20Gears%20of%20War%203%20(TU6%20Disc%3B%20TU1%20XBL).patch.toml (hash 7775898137CE1CE3)
   - Gears 3 base: https://github.com/xenia-canary/game-patches/blob/main/patches/4D5308AB%20-%20Gears%20of%20War%203.patch.toml (hash EF4AD5E9DD1982ED)
   - Judgment TU4: https://github.com/xenia-canary/game-patches/blob/main/patches/4D530A26%20-%20Gears%20of%20War%20Judgment%20(TU4).patch.toml ; base: .../4D530A26%20-%20Gears%20of%20War%20Judgment.patch.toml
   Facts only - we write our own patches from the addresses' meaning, not their bytes.
3. Ready-made regression list from the trackers [V] (Gears 3 https://github.com/xenia-canary/game-compatibility/issues/39 , Judgment .../issues/45):
   - Gears 3: green corruption + AO artifact at 2x/3x resolution scale (fine at 4x, or threshold 364/512) -> test our AO at non-integer scales; orange squares round fire particles; lights / lens flares through walls (occlusion queries); green speckles on Vulkan only; device lost in the Anvil Gate cutscene (Vulkan worked); "Previously on" videos in single-digit fps.
   - Judgment: textures go black / normal-map-looking after 10-20 min [G: texture cache eviction]; over-bright scenes (Aftermath) fixed by readback_resolve fast/full -> resolve readback; lights through walls; stutter fixed by async shaders.
4. Frame rate risk [V]: Gears 3 checkpoint / script soft-locks after a reload (Berserker cutscene missing, valves, Act 4 Ch 5 spawns, boss in mid-air, co-op scripts on P1); one user said Unlock FPS OFF helped. For us: keep 30 fps "(original)" as the default and test checkpoint reloads at 30 vs 60+. Cost: medium (test time).

### Other ports (features only, no code)
- OverkillLabs Judgment port, ReXGlue 0.10 + D3D12 (https://github.com/OverkillLabs2/gears-judgment-recomp) [V]: separate settings app with Play; folder picked once; raw-mouse keyboard/mouse; 60 fps pacing default, unlock option (90-110 fps on a 5800X3D, CPU-bound); SMAA, FSR1, auto quality by GPU; disk pipeline cache; F4 overlay + TOML; canary patches as settings. Known issues: above 2x internal res breaks, no menu cursor, thin-geometry shimmer (no TAA), no online.
- KiltedKernel Gears 3 port (XenonRecomp + Xenia D3D12, GPL-3.0, https://github.com/KiltedKernel/gears-of-war-3-pc-port) [V]: setup wizard (ISO, version check, winget build tools), launcher up to 240 fps, mod folders with mod.ini priority and automatic table-of-contents merge (mods can add chapters), System Link over LAN / friend code / relay server (Versus, Horde, co-op; Beast/Versus offline with bots). Same for its Gears 2 port.
- GloriousEggroll squarehole360 (ReXGlue + native Vulkan renderer, https://github.com/GloriousEggroll/squarehole360) [V]: Gears 3 playable on Linux through Act 1; draws captured into real render targets (not EDRAM emulation), background pipeline builds, planning / recording / submit split across threads; FSR 3.1, DLSS, XeSS, HDR10, VRR, System Link lobby + relay. Author: the command processor is now the bottleneck, not shaders; ~2.6 ms typical, 12 ms on the worst pans (RX 9070 XT, 720p -> 4K). Direction matches our layer 0.
- "Jacinto" Gears 3 PC build + DryreL Multipatch (https://www.nexusmods.com/gearsofwar3/mods/27) [V features]: soft-lock fixes (Mother Corpser, final Tempest), texture streaming pool 145 MB -> 8 GB, ultrawide / FOV, Steam co-op. Press says it is built from LEAKED SOURCE [S, https://www.xataka.com.mx/videojuegos/fans-gears-of-war-lograron-hazana-tomaron-codigo-filtrado-para-convertirlo-version-que-nunca-hizo-microsoft] - feature list only, never its files. Note the pool figure: our --gow3_texture_pool_mb caps at 400 today (falls back above) - worth revisiting [G].
- Split-screen co-op via several Xenia netplay instances: https://github.com/xSirLalo/xenia-coop-gears-of-war [V].

### Title updates
- Gears 3 TU6 is the last disc update [S] (patch file name; no patch notes found). TU4 (Mar 2012) was MP / Horde fixes [V, https://co-optimus.com/article/7888/fourth-gears-of-war-3-title-update-now-live-and-kicking.html]. Judgment's last is TU4 [S]; title ID 4D530A26 [V].

Parts 2 (UE3 on 360 rendering) and 3 (Gears 1) follow.

## Research 2 of 3 (Gears 3): how UE3 built frames on the 360, emulator pitfalls, settings, quality beyond 1x

From Gears 3, 21:50. Information, not an order. Web research (an agent's reading). Marks: [V] read in the source, [S] stated by a source, [G] guess. Little is public about Gears 3 / Judgment specifically; most is UE3 docs (UDK) and Xenia's write-ups.

### Ties to what we already measured
- Our Vulkan draw 303 (red-only into 8888_GAMMA, blend src*DEST_COLOUR) fits UE3's "attenuation buffer" for modulated shadows [V docs, G that 303 is it]: shadows drawn into a buffer that later multiplies scene colour. The docs say "normal" shadows are very expensive on 360 because other targets must be resolved out and restored - so this pass depends on exact EDRAM save / restore. Authentic looks NOT to fix: overlapping shadows darker, shadows through walls, emissive darkened. https://docs.unrealengine.com/udk/Three/ModulatedShadows.html
- 16_16 / 16_16_16_16 are fixed-point -32..32, not float; storing value/32 breaks multiply blending (1*1 -> 1/1024) -> black / far too dark blends [V, https://xenia.jp/updates/2021/04/27/leaving-no-pixel-behind-new-render-target-cache-3x3-resolution-scaling.html]. Matches the 9b lesson in our playbook (store UNorm(value)).

### 1. How frames were built [V unless marked]
- Gears 2 SSAO (GDC 2009, Smedberg / Wright): reduced resolution, smoothed over time with the previous frame's result - needs a history target that survives frames and stays paired with depth. https://www.gamedeveloper.com/game-platforms/previewing-gdc-2009-inside-the-programming-track , https://www.realtimerendering.com/blog/?p=89
- Light environments: characters' lights folded into ambient + one directional; one composite modulated shadow; updates rate-limited (stepping shadows are original). https://docs.unrealengine.com/udk/Three/LightEnvironments.html
- Dominant lights (UE3 2009+, so likely Gears 3 / Judgment): distance-field shadows, merged into the main pass on consoles, character "preshadows" from the world - more shadow-depth passes per character. https://docs.unrealengine.com/udk/Three/DominantLights.html
- Gears 3 overall: Lightmass GI, filmic look, radial blur, lens flares [S, https://www.cgw.com/Publications/CGW/2012/Volume-35-Issue-2-Feb-Mar-2012-/Gears-of-War-3.aspx].

### 2. 360 path vs PC path
- Predicated tiling: 720p+MSAA does not fit 10 MB EDRAM; the command buffer is replayed per tile and each tile resolved. For layer 0: merge tiles into one full-size target, skip the repeated vertex work. https://www.slideshare.net/slideshow/next-generation-graphics-programming-on-xbox-360/43938178 (see also Gears 1's canary "GUseTilingCode off / GTilingMode=4" in research part 3).
- MSAA in UE3 360 relied on Xenos' extended MSAA (PC needed DX10) - per-sample resolves must be right [V, https://www.shacknews.com/article/49793/gears-of-war-pc-review].
- Textures: DXN -> BC5, DXT5A -> BC4 direct; CTX1 needs a decode (to RG8 / BC5), one-off cost. https://docs.unrealengine.com/udk/Three/NormalMapFormats.html
- SimpleDOF is the console quarter-res version; BokehDOF is DX11 only (not in our games). https://docs.unrealengine.com/udk/Three/PostProcessEffectReference.html

### 3. Emulator pitfalls with symptoms [V, Xenia RT-cache post above unless marked]
- PWL gamma: 360 blends in linear with a 4-segment gamma curve; converting in the pixel shader blends in gamma space -> translucents / decals too dark or bright; real sRGB instead -> whole game too bright. Exact needs ROV-style blending (medium-high cost); sRGB is cheap but approximate. UE3 scene colour blends linearly, so it bites UI / 8-bit targets most. (Valve on the 360 curve: https://cdn.steamstatic.com/apps/valve/2008/GDC2008_PostProcessingInTheOrangeBox.pdf)
- 7e3 (2:10:10:10 float, 0..31.875): stand in RGBA16F and clamp to 31.875 to match. Cheap.
- float24 depth: truncating between passes -> dark stripes, shadow acne; keep full D32F on every round trip (matters for UE3 shadow projection). https://github.com/xenia-project/xenia/wiki/ROV
- Resolve / EDRAM aliasing and tile-rounded resolve sizes -> e.g. Halo 3 bloom black when a 72x40 copy became 96x40. UE3's quarter-res bloom / DOF and reduced-res AO chains are exactly such small odd-sized resolves.
- Eye adaptation needs GPU->CPU readback; too bright / dark pictures often come from it [S] (cf. Judgment's over-bright Aftermath fixed by readback_resolve, part 1).
- Half-pixel offset is a per-draw render state; wrong -> blurry / shifted post, 1-px edge gaps, more acne. https://www.aras-p.info/blog/2016/04/08/solving-dx9-half-pixel-offset
- Memexport / alpha-to-coverage: nothing UE3-specific found [G: missing memexport = missing particles or skinning].

### 4. SystemSettings a 360 build could still honour (https://docs.unrealengine.com/udk/Three/SystemSettings.html)
MaxAnisotropy; Max/MinShadowResolution, ShadowTexelsPerPixel; MotionBlur, DepthOfField, Bloom, AmbientOcclusion, Distortion; ScreenPercentage / UpscaleScreenPercentage; MaxMultiSamples; DynamicShadows / LightEnvironmentShadows; SkeletalMeshLODBias / ParticleLODBias; DetailMode, UseHighQualityBloom; TEXTUREGROUP_* LODBias / MaxLODSize / MinMagFilter (https://docs.unrealengine.com/udk/Three/TextureSupportAndSettings.html). [G] Shadow resolution is the safest visible win (EDRAM may cap it); aniso / LOD better done in our sampler; quality toggles that need shader variants the 360 never compiled will mostly not work. We already have the ini-edit overlay to try these (notes/25).

### 5. Quality beyond 1x - traps
- Bloom kernel defined at 1280 wide and scales with width; motion-blur MaxVelocity relative to width; DOF kernel in PIXELS -> ~half as strong at 2x; AO thresholds in world units (safe) [V, https://docs.unrealengine.com/udk/Three/Bloom.html , https://docs.unrealengine.com/udk/Three/MotionBlur.html].
- [G] The game computes post-process offsets on the CPU for 1280x720 - rescale size-related constants or bloom / DOF come out thin and small resolves misalign (fits the canary "Resolution Scaling Fix" in part 1).
- [G] 16x aniso in the sampler is the cheapest big win - but not for screen-sized post samples or shadow / depth lookups. Negative LOD bias only with real supersampling. Keep the AO history at a fixed fraction of the main target or it ghosts.
- Xenia's 3x3 scale needs about an RTX 3080 for 30 fps - mostly emulation overhead; a native path should do far better [V/G].

That completes Gears 3's part. I'll keep the three parts in our notes (notes/27) for the D3D12 work.

## Research 3 of 3 (Gears 3): Gears of War 1 - 360 build on Xenia, the 2007 PC version, what a port needs

From Gears 3, 21:45. Information, not an order. Web research (an agent's reading). Marks: [V] read in the source, [S] stated by a source, [G] guess. Part 2 (UE3 on 360 rendering) follows.

### Most useful for us
1. Canary's Gears 1 patch file is a map of what a port has to change [V]: https://github.com/xenia-canary/game-patches/blob/main/patches/4D5307D5%20-%20Gears%20of%20War.patch.toml (title ID 4D5307D5, default.xex module hash 1B591620508434A2, no TU named; media ID 76E9DF5B commented out). Patches (all off by default, facts only, we write our own):
   - 60 fps: present interval -> 1; physics and audio break above ~100 fps. -> cap / fix timing.
   - "Black Shading Fix": GUseTilingCode off, GTilingMode=4 - black characters from EDRAM predicated tiling. [G] Forcing UE3's non-tiled path at boot may remove a whole class of render bugs in all our games - worth asking whether Gears 2/3/Judgment have the same globals. Low cost to check (names in the exe / packages).
   - Decal flicker: DepthBias clamped at two load sites -> clamp depth bias in layer 0.
   - Texture streaming: pool 196, removes a 3 MB/frame limit, FudgeFactor 1.0 -> same idea as our --gow3_texture_pool_mb.
   - Coalesced hash check off - editing Coalesced.ini otherwise gives the dirty-disc screen (Gears 1 checks the packed config, not TU hash tables like Gears 2/3 [G]).
   - Motion blur off, all post off, 16x aniso, frame-time overlay; 21:9 / 16:10 (ConstrainedAspectRatio off, aspect float at 0x821367E8 = 1.777, buffer / UI viewport sizes, FOV hook).
2. Gears 1 on Xenia [V] (https://github.com/xenia-canary/game-compatibility/issues/44 , https://github.com/xenia-project/game-compatibility/issues/169): playable start to end on D3D12; Vulkan unstable. Left: decal / blood z-fighting (ROV path accurate but slow), black polygons on cutscene characters, lens flares through walls (occlusion queries), audio stuck in the prison-cell section (XMA settings), checkerboard flashes in the tutorial; [S] car-push scene loses distance on each checkpoint retry. Xenia Manager puts it with the UE3 games needing xenos_sample_positions=true and window_offset_in_edram=false (MSAA tiling corruption) [V, https://github.com/xenia-manager/optimized-settings/pull/77] - for layer 0: 360 MSAA sample positions and EDRAM offsets must match.
3. Existing port: SomeoneIsWorking/gears1 (https://github.com/SomeoneIsWorking/gears1) [V] - Xenia Canary fork (not ReXGlue), partial native renderer, reaches Act 1 at 30 Hz per README, launcher currently refuses to start; ~823 commits; one exact retail revision. Facts only.

### The 2007 Windows version (reference for UE3 PC settings)
- Certificate expiry broke it on 28 Jan 2009 (not 2019); fixed by a Feb 2009 patch / TU3 [V, https://www.pcgamingwiki.com/wiki/Gears_of_War , https://shacknews.com/article/57039/epic-explains-gears-of-war]. For us: never ship a date / signature check.
- Capped at 62 fps; `set Engine.GameEngine bSmoothFrameRate FALSE` uncaps, and then pushable cars move much slower - physics tied to frame time [V, PCGW]. Same risk class as Gears 3's checkpoint soft-locks.
- FOV: `set WarfareGame.WarCameraMode FOVAngle N` [V, PCGW] - Gears 1's game package is WarfareGame (Gears 3: GearGame), and like Gears 3 the FOV is on the camera mode.
- Config: My Games\Gears of War for Windows\WarGame\Config\ - WarEngineUserSettings.ini MotionBlur; WarInput.ini mouse smoothing; [TextureLODSettings] TEXTUREGROUP_World / Character / Weapon / Vehicle / Effects / Skybox / UI / LightAndShadowMap / RenderTarget (+ NormalMap / Specular) with MinLODSize / MaxLODSize / LODBias [S, https://www.gamersky.com/handbook/200711/83841.shtml]. These are the texture-quality knobs a launcher preset could set through our ini-edit overlay.
- Wider UE3 SystemSettings names (UDK docs, later than 2007) [V, https://docs.unrealengine.com/udk/Three/SystemSettings.html]: DynamicShadows, LightEnvironmentShadows, Min/MaxShadowResolution, ShadowTexelsPerPixel, ShadowFilterRadius, ShadowDepthBias, MotionBlur, DepthOfField, Bloom, UseHighQualityBloom, Distortion, AmbientOcclusion, MaxAnisotropy, MaxMultiSamples, DynamicDecals, DecalCullDistanceScale, FogVolumes, ScreenPercentage, UpscaleScreenPercentage, UseVSync.
- PC extras (five Act 5 chapters with the Brumak, King of the Hill, 3 maps, editor) are PC-only data - out of scope. DX10 gave almost nothing; AA only in DX10 (4x MSAA). GFWL (XLiveless removes it) [V].
- Ultimate Edition (2016, UE3 + some UE4 shading, DX12): 4K, uncapped; launch had up-to-10 s stutters at level transitions, AMD AO bugs, G-Sync problems [V, https://en.wikipedia.org/wiki/Gears_of_War:_Ultimate_Edition , https://www.pcgamer.com/uk/gears-of-war-ultimate-edition-is-still-having-performance-issues]. Players expect 4K and uncapped; streaming hitches hurt more than missing effects [G].
- Fan "Gears of War: Redux" of the PC version (4K, ultrawide, FOV, Horde) [S] - a sign of what players want.

### What a Gears 1 port needs that differs [G unless marked]
- Older UE3 than Gears 2; class names under WarfareGame [V]; do not assume Gears 2/3 names or offsets.
- One exact build: 360 TU of 14 Jun 2007 (Annex achievements, roadie-run changes, MP fixes [V, https://gamingnexus.com/News/4749/Gears-of-War-update-and-new-achievements]) vs base - patch addresses match only one; canary's file is the base xex.
- Coalesced.ini hash check instead of TU hash tables (our package-patch path would need a hash-check stub for it).
- 16:9-locked main menu (ConstrainedAspectRatio); System Link / co-op to replace as for Gears 2/3.


## Follow-up: Gears 1's tiling switch in Gears 2 / 3? (21:55, local files only)
- The names GUseTilingCode / GTilingMode are not in the Gears 3 or Gears 2 exe images nor in Gears 3's Coalesced_INT (ASCII and UTF-16 searched) [V]. They are C++ globals with no name in a retail exe; canary's Gears 1 patch names them in comments only. The one "tiling" string in Gears 3's exe is a D3D debug message ("resource is being locked while in a tiling, Z-pass or XPS bracket") [V] - so the 360 D3D tiling brackets are there.
- Finding the matching globals in Gears 2 / 3 would need the code that reads them (e.g. the BeginTiling call site in the scene renderer) - a disassembly job, not a name search. Not started.

## Regression tests from Gears 2's D3D12 report (mail 21:44) [S, Xenia source comments]
- Gears 3: memexport to k_8_8_8_8_A in an 8192-point draw between the intro video and the menu; discrete tessellation for character faces in campaign cinematics; k_DXT3A_AS_1_1_1_1 lights the T-beams early in mission 1 (component order a TODO in Xenia).
- Judgment: post-process data written into the stencil byte of an 8_8_8_8 target while reading depth; resolve height includes source_top (scissor must clip resolves).
- Gears 1: UE3 MSAA viewport offsets assume the 360's sample positions (depth restore fails otherwise). Gears 2: rectangle-list backface culling breaks it.
- Note: the report gives Gears 3 as 4D53085B; ours is 4D5308AB (notes/01) - flagged to Gears 2.
