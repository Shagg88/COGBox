# Third-party notices

Gears2-Native's own code is MIT-licensed (`LICENSE`). It contains or builds against:

| Component | Where | Licence |
|---|---|---|
| rexglue SDK (Tom Clay; portions from the Xenia project, Ben Vanik and contributors) | `third_party/rexglue-sdk` (downloaded, not committed); `cmake/rexglue.cmake`, `src/main.cpp`, parts of `src/gow2_app.h` come from its project template | BSD-3-Clause |
| Xenia Canary (e6cd2e4) and Xenia Edge (5045c19) (Ben Vanik and the Xenia project contributors) | ported or adapted pieces of `src/layer0_xenos` (texture loading and formats, tiling and mip layout, resolve and gamma rules, the rectangle-list geometry shader, PM4 and shader-translation rules); each such file carries the notice | BSD-3-Clause |
| XeGTAO (Intel Corporation) | `src/layer0_xenos/renderer/xegtao_shaders.h` (ported shaders, notes/80) | MIT (notice below) |
| PsychoV35 (Carlos Lopez / ShortFuse / clshortfuse; self-contained edition modifications by Musa Haji / mqhaji) | Gears 2 and Gears 3 native HDR scene mapping, `src/layer0_xenos/include/xenos/renderer/psychov35_source.h` | MIT ([full notice](cogbox/licenses/PsychoV35-LICENSE.txt); also copied beside each game EXE under `licenses/`) |
| FidelityFX CACAO (Advanced Micro Devices; ASSAO, Intel Corporation) | `src/layer0_xenos/renderer/cacao_shaders.h` (ported shaders, notes/81) | MIT (notice below) |
| Libraries bundled by the SDK (SDL3, Dear ImGui, Tracy, fmt, spdlog, FFmpeg, ...) | inside the SDK | their own licences, listed in the SDK's `licenses/` folder |
| gow3-recomp (Flippage and the gow3-recomp contributors) and gears-of-war-2-pc-port / gow2-recomp (Santiago Quiroz Upegui and the gow2-recomp contributors) | the native mouse look method and the Gears 3 / Gears 2 base-disc addresses in `ue3_mouse_look.h` and `mouse_look.cpp` (layer 3 of all three ports, COGBox 0.2.1), plus the multi-controller 4x prompt overlay catalog and live device monitor (`prompt_atlas_catalog.*`, `prompt_overlay_compositor.*`, `prompt_device_monitor.*`) | BSD-3-Clause (notices below) |
| Kenney Input Prompts (`assets/glyphs/`) | Controller and keyboard/mouse prompt glyph icons for Xbox, PlayStation, Nintendo Switch, Steam Deck, and Steam Controller | CC0 1.0 Universal (Public Domain, notice below) |
| Inter 4.1 (The Inter Project Authors, `assets/fonts/`) | Inter-Regular.ttf and Inter-SemiBold.ttf, the achievement popups' and the in-game Achievements page's font | SIL Open Font License 1.1 (`assets/fonts/Inter-LICENSE.txt`) |
| LZMA SDK 23.01 7z decoder (Igor Pavlov, `cogbox/third_party/lzma`, unchanged) | CogBox.exe unpacking .7z updates (0.4.9) | Public domain (`cogbox/licenses/LZMA-SDK-public-domain.txt`) |
| miniupnpc 2.3.3 (Thomas Bernard; release tarball fetched by SDK patch 77, pinned by SHA-256, built into `rexruntime.dll`) | UPnP port mapping for online play (0.6.0) | BSD-3-Clause (`cogbox/licenses/miniupnpc-LICENSE.txt`) |
| nlohmann/json 3.11 (Niels Lohmann; `cogbox/third_party/nlohmann`, and the copy inja carries inside the SDK for patches 78 / 79) | The launcher's and the runtime's online-play messages (0.6.0) | MIT (`cogbox/licenses/nlohmann-json-LICENSE.txt`) |

No game files are included. Players supply their own copy of Gears of War 2.
Vendor upscalers ship as the vendors' own binaries next to the exe, under their own terms, and their SDKs are never
committed (`third_party/` is ignored; `notes/65-broad-hardware.md` item 6):

| Component | Used for | Licence |
|---|---|---|
| NVIDIA DLSS SDK (`third_party/dlss-sdk`; `nvngx_dlss.dll` at run time) | DLSS / DLAA | NVIDIA RTX SDKs License (binary redistribution inside an application; the SDK must not be made subject to an open-source licence, so it stays a separate binary next to our MIT code) |
| AMD FidelityFX SDK (`third_party/fidelityfx-sdk`; `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll`) | FSR | The `ffx_api` headers we compile against: MIT (AMD). The signed DLLs: AMD's FidelityFX licence (`docs/license.md`) - redistribution in binary form only, no reverse engineering, and its copyright notice, permission notice and disclaimers reproduced (below) |
| Intel XeSS SDK 3.0.2 (`third_party/xess-sdk-3.0.2`; `libxess.dll` 2.0.2.68 (the XeSS Super Resolution library as shipped), and from 0.4.9 `libxess_fg.dll` 1.3.1.78 + `libxell.dll` 1.3.2.10 beside `gow2.exe`) | XeSS upscaling; experimental Intel XeSS frame generation with Xe Low Latency (`renderer/xess_framegen.cpp`, `--gow2_frame_generation=xess`) | Intel Simplified Software License (October 2022) - binary-only redistribution without modification, notice reproduced (below); the SDK's `third-party-programs.txt` ships in `licenses\xess-sdk-3.0.2\`. Intel, Arc and XeSS are trademarks of Intel Corporation; no endorsement by Intel is implied |
| NVIDIA Streamline SDK v2.14.16 headers (`src/layer0_xenos/streamline/include`, vendored unchanged; licence in `src/layer0_xenos/streamline/LICENSE.txt`) | Experimental DLSS frame generation (`renderer/dlss_framegen.cpp`, `--gow2_frame_generation=dlss`) with Reflex through Streamline | Headers: MIT (NVIDIA). Shipped unmodified from 0.4.8 beside each port exe built with Streamline: `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll` (Streamline 2.14, MIT, NVIDIA-signed; Reflex parts under the Reflex SDK licence) and `nvngx_dlssg.dll` (NVIDIA RTX SDKs License). Release `licenses\NVIDIA-Streamline-DLSS-G-NOTICE.txt`, `licenses\streamline-sdk\`, `licenses\streamline-release\`. This product uses the NVIDIA DLSS SDK, NVIDIA Streamline SDK and NVIDIA Reflex SDK; NVIDIA, GeForce RTX, DLSS and Reflex are trademarks of NVIDIA Corporation |
| SPIRV-Tools (Khronos Group; built from the rexglue SDK's vendored source, `third_party/rexglue-sdk/thirdparty/spirv-tools`) | Vulkan: the optimiser run over each shader after it is converted to SPIR-V (0.6.1) | Apache-2.0 (`LICENSE` in that folder; shipped with the SDK licences) |

A release that ships these DLLs must include the notices below (and NVIDIA's DLSS attribution when DLSS ships).

### AMD FidelityFX SDK (binaries)

Copyright (C) Advanced Micro Devices, Inc.

REDISTRIBUTION: Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to install, reproduce, copy and distribute copies of the Software, in
binary form only, and to permit persons to whom the Software is provided to do the same, provided that the following
conditions are met:

No reverse engineering, decompilation, or disassembly of this Software is permitted.

Redistributions must reproduce the above copyright notice, this permission notice, and the following disclaimers and
notices in the Software documentation and/or other materials provided with the Software.

DISCLAIMER: THE USE OF THE SOFTWARE IS AT YOUR SOLE RISK. THE SOFTWARE IS PROVIDED "AS IS" AND WITHOUT WARRANTY OF ANY
KIND AND COPYRIGHT HOLDER AND ITS LICENSORS EXPRESSLY DISCLAIM ALL WARRANTIES, EXPRESS AND IMPLIED, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. COPYRIGHT
HOLDER AND ITS LICENSORS DO NOT WARRANT THAT THE SOFTWARE WILL MEET YOUR REQUIREMENTS, OR THAT THE OPERATION OF THE
SOFTWARE WILL BE UNINTERRUPTED OR ERROR-FREE. THE ENTIRE RISK ASSOCIATED WITH THE USE OF THE SOFTWARE IS ASSUMED BY
YOU. FURTHERMORE, COPYRIGHT HOLDER AND ITS LICENSORS DO NOT WARRANT OR MAKE ANY REPRESENTATIONS REGARDING THE USE OR THE
RESULTS OF THE USE OF THE SOFTWARE IN TERMS OF ITS CORRECTNESS, ACCURACY, RELIABILITY, CURRENTNESS, OR OTHERWISE.

DISCLAIMER: UNDER NO CIRCUMSTANCES INCLUDING NEGLIGENCE, SHALL COPYRIGHT HOLDER AND ITS LICENSORS OR ITS DIRECTORS,
OFFICERS, EMPLOYEES OR AGENTS ("AUTHORIZED REPRESENTATIVES") BE LIABLE FOR ANY INCIDENTAL, INDIRECT, SPECIAL OR
CONSEQUENTIAL DAMAGES (INCLUDING DAMAGES FOR LOSS OF BUSINESS PROFITS, BUSINESS INTERRUPTION, LOSS OF BUSINESS
INFORMATION, AND THE LIKE) ARISING OUT OF THE USE, MISUSE OR INABILITY TO USE THE SOFTWARE, BREACH OR DEFAULT,
INCLUDING THOSE ARISING FROM INFRINGEMENT OR ALLEGED INFRINGEMENT OF ANY PATENT, TRADEMARK, COPYRIGHT OR OTHER
INTELLECTUAL PROPERTY RIGHT EVEN IF COPYRIGHT HOLDER AND ITS AUTHORIZED REPRESENTATIVES HAS BEEN ADVISED OF THE
POSSIBILITY OF SUCH DAMAGES. IN NO EVENT SHALL COPYRIGHT HOLDER OR ITS AUTHORIZED REPRESENTATIVES TOTAL LIABILITY FOR
ALL DAMAGES, LOSSES, AND CAUSES OF ACTION (WHETHER IN CONTRACT, TORT (INCLUDING NEGLIGENCE) OR OTHERWISE) EXCEED THE
AMOUNT OF US$10.

### Intel XeSS SDK (binaries)

The full text is `LICENSE.txt` in the SDK; a release copies it to `licenses\xess-sdk-3.0.2\`. It covers `libxess.dll`, `libxess_fg.dll` and `libxell.dll` (all three are in the same SDK). Its notice:

Intel Simplified Software License (Version October 2022)

The native HDR UE3 grading bridge uses RenoDX reference strategies, adapted
Oklab matrices and both games' smoothed-power HDR boost from the Dishonored
sample, copyright (c) 2025 Carlos Lopez Jr. / ShortFuse. Its MIT notice
is provided in `cogbox/licenses/RenoDX-Grading-LICENSE.txt` and distributed beside
both game executables. See `../docs/ue3-hdr-grading.md` for upstream references.

Intel(R) Xe Super Sampling (XeSS) SDK: Copyright (C) 2025 Intel Corporation

Use and Redistribution. You may use and redistribute the software, which is provided in binary form only, (the
"Software"), without modification, provided the following conditions are met:

* Redistributions must reproduce the above copyright notice and these terms of use in the Software and in the
  documentation and/or other materials provided with the distribution.
* Neither the name of Intel nor the names of its suppliers may be used to endorse or promote products derived from
  this Software without specific prior written permission.
* No reverse engineering, decompilation, or disassembly of the Software is permitted, nor any modification or
  alteration of the Software or its operation at any time, including during execution.

