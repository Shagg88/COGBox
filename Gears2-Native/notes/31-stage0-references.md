# 31 - Stage 0 reference facts (2026-10-20)

Research done in a second chat at the user's request and pasted back here. Labels: everything below was checked by that chat
against the named sources (it reproduced Brief 1's error, built and ran FLIP, and compiled the Brief 3 algorithms).
Checked again in this chat: the Canary `Float7e3To32` really places the mantissa with `<< 16` (`xenos.cc` ~line 119, Canary
e6cd2e4) and `Tiled2D` is at `texture_address.h:190`; a restatement of the 7e3 and Tiled2D algorithms reproduces every
example value listed below.

**Reference emulator for Xenos behaviour: Xenia Canary (`canary_experimental`) or Xenia Edge (has207/xenia-edge), never
xenia-project master** - master's `Float7e3To32` still has the mantissa bug fixed in Canary f3e42609a (4 Aug 2026).
Canary and Edge have identical CPU conversion code (`xenos.cc`, `pixel_formats.xesli`, `texture_address.h`); Edge has no DXBC
shader translator, so the float32->7e3 encoder reference is the SPIR-V one (`spirv_shader_translator_rb.cc`:
`PreClampedFloat32To7e3` line 25, `UnclampedFloat32To7e3` line 109, `Float7e3To32` line 139), identical in both.

Credit line for tests: "Expected values derived from Xenia Canary / Xenia Edge, Copyright (c) Ben Vanik and Xenia project
contributors, BSD 3-Clause License."

**How each item is used here:**
- Brief 1 -> symbolized SDK crash stacks: install the RelWithDebInfo SDK and ALSO `cmake --install --config Release` into the
  same prefix (tested workaround), or the two-line SDK fix (candidate for the upstream patch list).
- Brief 2 -> the harness: FLIP CLI flags/outputs (CSV, `-et -etp` exit codes, fixed `-cstart/-cstop/-n` for HDR so scores
  compare across frames); EXR writing with tinyexr v1 (+ miniz compiled as C), since DirectXTex needs OpenEXR for EXR.
- Brief 3 -> Catch2 unit tests for layer 0 format code.

## Brief 1: rexglue RelWithDebInfo install

**Cause (verified in source, `nightly-20261002-bd833a2a`):**
- **The tool is installed for Release only.** `cmake/rexglue_install.cmake` lines 13–18 install it that way: `install(TARGETS rexglue EXPORT rexglueTargets RUNTIME … CONFIGURATIONS Release)`, with the comment "only Release ships".
- **A RelWithDebInfo install therefore has no `rexglue` binary.** The exported `rexglueTargets.cmake` still declares `add_executable(rex::rexglue IMPORTED)`, but `rexglueTargets-relwithdebinfo.cmake` has no entry for it.
- **The SDK's own mapping points at a config that isn't there.** `cmake/rexglueConfig.cmake.in` lines 41–47 set `MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release`, and no Release config exists in that prefix.
- **The DLL name is unrelated.** `rexruntimerd.dll` comes from `CMAKE_RELWITHDEBINFO_POSTFIX rd` (`CMakeLists.txt` 106–115) and is correct.

**Verified by reproduction:** a minimal model of these rules on CMake 3.28 with Ninja Multi-Config gives exactly `IMPORTED_LOCATION not set for imported target "rex::rexglue" configuration "RelWithDebInfo"`.

**A mapping on the downstream side cannot fix it.** I tested `MAP_IMPORTED_CONFIG_RELWITHDEBINFO "RelWithDebInfo;Release"` downstream and it still fails, because there is no binary to map to.

**Smallest fix, in the SDK (two edits):**

```cmake
# cmake/rexglue_install.cmake
install(TARGETS rexglue
    EXPORT rexglueTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    CONFIGURATIONS Release RelWithDebInfo)
```
```cmake
# cmake/rexglueConfig.cmake.in, replacing the existing block
if(TARGET rex::rexglue)
    set_target_properties(rex::rexglue PROPERTIES
        MAP_IMPORTED_CONFIG_DEBUG          "Release;RelWithDebInfo"
        MAP_IMPORTED_CONFIG_RELEASE        "Release;RelWithDebInfo"
        MAP_IMPORTED_CONFIG_RELWITHDEBINFO "RelWithDebInfo;Release"
        MAP_IMPORTED_CONFIG_MINSIZEREL     "Release;RelWithDebInfo")
endif()
```

How it behaves:
- **No overwrite.** RelWithDebInfo is optimised, so the "Debug is too slow" reason doesn't apply. `rex_config_postfix(rexglue)` names it `rexgluerd.exe`, so it never overwrites a Release `rexglue.exe` in the same prefix.
- **Tested (modelled):** a RelWithDebInfo-only install works for downstream Debug, Release and RelWithDebInfo, and a Release-only install still works.

**Workarounds with no SDK change (both tested):**
- Also run `cmake --install <build> --config Release` into the same prefix, after building Release. The existing mapping then resolves.
- Or point the target at an existing tool downstream:
  ```cmake
  find_package(rexglue CONFIG REQUIRED)
  set_property(TARGET rex::rexglue APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
  set_target_properties(rex::rexglue PROPERTIES IMPORTED_LOCATION_RELEASE "${REXGLUE_TOOL}")
  ```

**Not checked (guess):** the Visual Studio generator on Windows. Install/export rules behave the same across generators, but I tested on Linux.

---

## Brief 2: FLIP and EXR

### FLIP v1.7 (NVlabs/flip, HEAD d3ab02a, 13 Oct)

**Build, CPU only.** I built it with clang + Ninja here. Per the README, the source root is `src/`:

```
cmake -S flip/src -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build build        # produces build/flip(.exe)
```

- **CUDA:** off by default (`option(FLIP_ENABLE_CUDA … OFF)`).
- **OpenMP:** optional. `find_package(OpenMP)` is linked only if it's found. Without it the tool is single-threaded: 1.5 s for an LDR pair and about 14 s for an HDR pair at 1920×1080 here.
- **Windows:** clang-cl or clang++ both work with CMake. To get OpenMP, install LLVM's libomp and ship `libomp.dll` next to the tool (not tested on Windows).

**Flags** (`src/cpp/tool/commandline.h` lines 129–161). Long form is `--name`, short form is `-x`:
- `-r/--reference`, `-t/--test` (one or more). Both must be the same resolution and format.
  - **PNG gives LDR-FLIP; EXR gives HDR-FLIP.** There's no switch: the file type decides.
- **HDR only:**
  - `-tm ACES|HABLE|REINHARD`
  - `-cstart`, `-cstop`, `-n` (exposures). Fix these for the harness; otherwise they're computed automatically from each reference, and scores aren't comparable across frames.
- **Viewing distance:** `-ppd` or `-vc`.
- **Output location and names:** `-d <dir>`, `-b <basename>`.
- **Machine-readable results:** `-c <file.csv>` and `-txt`.
- **What to save:** `-nerm` (no error map), `-nexm` (no exposure map), `-nm` (grayscale), `-hist`.
- **Harness pass/fail:** `-et -etp MEAN|WEIGHTED-MEDIAN|MAX <threshold>` makes the tool exit with failure when the quantity exceeds the threshold. Verified: it returned exit code 1.
- **Console detail:** `-v 0|1|2`.

**Outputs** (verified). There's no JSON:
- **Console:** Mean, Weighted median, 1st/3rd weighted quartile, Min, Max, evaluation time.
- **CSV** (appends rows, writes a header once): `"Reference","Test","Mean","Weighted median","1st weighted quartile","3rd weighted quartile","Min","Max","Evaluation time"`.
- **`-txt`:** writes `pooled_values.<…>.txt` with the same six numbers.
- **Error map:** `flip.<ref>.<test>.<ppd>ppd.ldr.png`, or `….hdr.<tm>.<cstart>_to_<cstop>.<N>.png` plus an exposure map for HDR.

### EXR writing

**DirectXTex (may2026 and HEAD 4234f77): no, not on its own.** `Auxiliary/DirectXTexEXR.cpp` says "Requires the OpenEXR library" (lines 24–26) and includes `ImfRgbaFile.h`. In `CMakeLists.txt`, `ENABLE_OPENEXR_SUPPORT` defaults to OFF and runs `find_package(OpenEXR REQUIRED)` when on.

**Lightest permissive option: tinyexr (BSD-3).** Its v1 single header is the same one FLIP uses to read EXR:
- **Files:** `tinyexr.h`, plus `deps/miniz/miniz.c`/`.h` for ZIP compression.
- **Compile miniz as C, not C++.** It fails under clang++; I verified this.
- **API:** `SaveEXR(const float* rgba, w, h, 4, /*save_as_fp16*/1, "out.exr", &err)` writes half-float RGBA with ZIP compression.
- **Verified end to end:** I wrote two such files, read them back with `LoadEXR`, and FLIP's HDR mode compared them.
- **Alternative:** `TINYEXR_USE_STB_ZLIB=1` uses stb's zlib instead of miniz.
- **Note:** the README now calls v1 "old, deprecated" in favour of v3, a C11 library (`include/exr.h` + `src/*.c`, vendored zstd/libdeflate). v1 is still the smallest drop-in.

---

## Brief 3: Xenos format reference (xenia-canary, BSD-3)

All functions are in `src/xenia/gpu/`. Example values come from compiling exact restatements of the code; I hand-checked 1.0, 0.25 and 31.875.

**1. 7e3 → float32**: `float xenos::Float7e3To32(uint32_t f10)`, `xenos.cc:104` (declared `xenos.h:416`).
- **Algorithm:** take the low 10 bits; 0 gives 0. mantissa = bits 0–6, exponent = bits 7–9. A denormal (exponent 0) is normalised with a leading-zero count. Result bits are `((exp+124)<<23) | (mantissa<<16)`. The range is [0, 31.875] with no sign.
- **Shader equivalent:** `XeUnpackR10FloatX4` (`shaders/pixel_formats.xesli`).
- **Examples:** 0x000→0, 0x001→0.001953125, 0x07F→0.248046875, 0x080→0.25, 0x0C0→0.375, 0x200→2, 0x380→16, 0x3FF→31.875.
- **Warning: master has a bug.** xenia-project/xenia master (95a5c3e, Feb 2026) uses `mantissa << 3`, so 0x0C0 gives 0.25002 and 0x3FF gives 16.002. Canary fixed it in **f3e42609a** ("Fix mantissa placement in CPU Float7e3To32", 4 Aug 2026), and edge and rexglue have the fix. Use canary, edge or rexglue as the reference.

**2. float32 → 7e3:** there's no CPU function; it exists only as shader emitters.
- **Where:** `DxbcShaderTranslator::PreClampedFloat32To7e3` (`dxbc_shader_translator_om.cc:3816`) and `UnclampedFloat32To7e3` (`:3875`), with matching SPIR-V versions.
- **Algorithm:** Unclamped does `max(x, 0)` then `min(·, 31.875)`, which is NaN-safe: NaN gives 0. Then:
  - if x < 0.25: `b = ((bits & 0x7FFFFF)|0x800000) >> min(125 − (bits>>23), 24)`;
  - otherwise `b = bits − (124<<23)`;
  - `f10 = ((b + 0x7FFF + ((b>>16)&1)) >> 16) & 0x3FF`, which rounds to nearest-even.
- **Examples:** 0→0x000, 0.25→0x080, 1.0→0x180, 1.5→0x1C0, 0.1→0x033 (decodes to 0.099609375), 0.0078125→0x004, 40→0x3FF, +Inf→0x3FF, −1→0x000, NaN→0x000.

**3. float32 → 20e4 depth**: `uint32_t xenos::Float32To20e4(float, bool round_to_nearest_even)`, `xenos.cc:126`. Shader twin: `XeFloat32To20e4`, `pixel_formats.xesli:652`.

- **Algorithm:**
  - Not > 0 (including −0 and NaN) gives 0. Bits ≥ 0x3FFFFFF8 give 0xFFFFFF.
  - Below 2⁻¹⁴ (bits < 0x38800000) is made denormal: `(0x800000|mant) >> min(113 − exp, 24)`. Otherwise add 0xC8000000, which rebiases the exponent by −112.
  - Optional round to nearest-even (`+3 + ((v>>3)&1)`), then `(v>>3) & 0xFFFFFF`. The range is [0, 2).
- **Examples (RNE / truncate):** 0.5→0xE00000, 1.0→0xF00000, 1.5→0xF80000, 0.3→0xD33333, 1.99999→0xFFFFF6 / 0xFFFFF5, 1e‑5→0x029F17 / 0x029F16, 2.0→0xFFFFFF, −1→0, NaN→0.

**4. 20e4 → float32**: `float xenos::Float20e4To32(uint32_t f24)`, `xenos.cc:151`.
- **Algorithm:** 24 bits; mantissa = bits 0–19, exponent = bits 20–23; denormals are normalised. Result bits are `((exp+112)<<23) | (mantissa<<3)`.
- **Shader version:** `XeFloat20e4To32` has an extra `remap_to_0_to_0_5` flag.
- **Examples:** 0x000001→5.82076609e‑11, 0x0FFFFF→6.1035098e‑05, 0x100000→6.10351562e‑05, 0x700000→0.00390625, 0xF00000→1.0, 0xFFFFFF→1.99999905.

**5. k_2_10_10_10 unpack:** shader-only, `pixel_formats.xesli`.
- **Functions:** `XeUnpackR10G10B10A2UNorm` (line 363) and `XeUnpackR10G10B10A2Float` (line 389, the 7e3 RGB variant). The packer is `XePackR10G10B10A2UNorm`: saturate, scale, +0.5, truncate.
- **Algorithm:** R = bits 0–9, G = 10–19, B = 20–29, A = 30–31. UNorm divides by 1023 (alpha by 3). The FLOAT variant decodes each RGB channel as 7e3 (as in item 1), with alpha/3.
- **Examples:**
  - 0xC00003FF → UNorm (1,0,0,1), FLOAT (31.875,0,0,1)
  - 0x40100401 → UNorm (0.000977517 ×3, 0.333333), FLOAT (0.001953125 ×3, 0.333333)
  - 0x3FF003FF → (1,0,1,0) / (31.875,0,31.875,0)
  - 0xFFFFFFFF → (1,1,1,1) / (31.875 ×3, 1)

**6. Tiled address**: `int32_t texture_address::Tiled2D(int32_t x, int32_t y, uint32_t pitch_aligned, unsigned bytes_per_block_log2)`, `texture_address.h:190`, using `TiledCombine` at `:174`. `Tiled3D` follows it.

- **Inputs:** x and y are in blocks, and pitch must already be aligned to 32 blocks.
- **Algorithm:**
  - `outer = ((y>>5)*(pitch>>5) + (x>>5)) << 6`
  - `inner = ((y>>1)&7)<<3 | (x&7)`
  - `oib = (outer|inner) << bpb_log2`
  - `bank = (y>>4)&1`, `pipe = ((x>>3)&3) ^ (((y>>3)&1)<<1)`
  - Combine: `(y&1)<<4 | pipe<<6 | bank<<11 | oib[3:0] | oib[4]<<5 | oib[7:5]<<8 | (oib>>8)<<12`
- **The header comment** (lines ~70–160) documents the bit layout and credits crunch and AMD AddrLib.
- **Examples:**
  - Pitch 1280, 32 bpp (log2 = 2): (0,0)→0, (1,0)→4, (7,0)→44, (8,0)→64, (0,1)→16, (0,2)→256, (0,8)→1152, (0,16)→2048, (32,0)→4096, (0,32)→163840, (100,50)→178464.
  - Pitch 32, DXT1 (log2 = 3): (1,0)→8, (0,1)→16, (31,31)→8056.

**Credit line for your tests:** "Expected values derived from Xenia (xenia-canary), Copyright (c) Ben Vanik and Xenia project contributors, BSD 3-Clause License."

Sources: github.com/rexglue/rexglue-sdk, github.com/NVlabs/flip, github.com/syoyo/tinyexr, github.com/microsoft/DirectXTex, github.com/xenia-canary/xenia-canary, github.com/has207/xenia-edge
