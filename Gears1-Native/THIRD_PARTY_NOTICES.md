# Third-party notices

Gears2-Native's own code is MIT-licensed (`LICENSE`). It contains or builds against:

| Component | Where | Licence |
|---|---|---|
| rexglue SDK (Tom Clay; portions from the Xenia project, Ben Vanik and contributors) | `third_party/rexglue-sdk` (downloaded, not committed); `cmake/rexglue.cmake`, `src/main.cpp`, parts of `src/gow2_app.h` come from its project template | BSD-3-Clause |
| Xenia Canary (e6cd2e4) and Xenia Edge (5045c19) (Ben Vanik and the Xenia project contributors) | ported or adapted pieces of `src/layer0_xenos` (texture loading and formats, tiling and mip layout, resolve and gamma rules, the rectangle-list geometry shader, PM4 and shader-translation rules); each such file carries the notice | BSD-3-Clause |
| Libraries bundled by the SDK (SDL3, Dear ImGui, Tracy, fmt, spdlog, FFmpeg, ...) | inside the SDK | their own licences, listed in the SDK's `licenses/` folder |
| gow3-recomp (Flippage and the gow3-recomp contributors) and gears-of-war-2-pc-port / gow2-recomp (Santiago Quiroz Upegui and the gow2-recomp contributors) | the native mouse look method and the Gears 3 / Gears 2 base-disc addresses in `ue3_mouse_look.h` and `mouse_look.cpp` (layer 3 of all three ports, COGBox 0.2.1) | BSD-3-Clause (notices below) |

No game files are included. Players supply their own copy of Gears of War 2.
Vendor upscalers ship as the vendors' own binaries next to the exe, under their own terms, and their SDKs are never
committed (`third_party/` is ignored; `notes/65-broad-hardware.md` item 6):

| Component | Used for | Licence |
|---|---|---|
| NVIDIA DLSS SDK (`third_party/dlss-sdk`; `nvngx_dlss.dll` at run time) | DLSS / DLAA | NVIDIA RTX SDKs License (binary redistribution inside an application; the SDK must not be made subject to an open-source licence, so it stays a separate binary next to our MIT code) |
| AMD FidelityFX SDK (`third_party/fidelityfx-sdk`; `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll`) | FSR | The `ffx_api` headers we compile against: MIT (AMD). The signed DLLs: AMD's FidelityFX licence (`docs/license.md`) - redistribution in binary form only, no reverse engineering, and its copyright notice, permission notice and disclaimers reproduced (below) |
| Intel XeSS SDK (`third_party/xess-sdk`; `libxess.dll`) | XeSS | Intel Simplified Software License (October 2022) - binary-only redistribution without modification, notice reproduced (below) |
| NVIDIA Streamline SDK (`third_party/streamline-sdk`) | planned: DLSS frame generation, Reflex, DLSS 5 (notes/65) | Its own source: MIT (NVIDIA). Its signed DLLs come from the release package; the DLSS feature DLLs it loads keep the NVIDIA RTX SDKs License. Listed in full when integrated |

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

The full text is `LICENSE.txt` in the SDK; a release copies it next to `libxess.dll`. Its notice:

Intel Simplified Software License (Version October 2022)

Intel(R) Xe Super Sampling (XeSS) SDK: Copyright (C) 2025 Intel Corporation

Use and Redistribution. You may use and redistribute the software, which is provided in binary form only, (the
"Software"), without modification, provided the following conditions are met:

* Redistributions must reproduce the above copyright notice and these terms of use in the Software and in the
  documentation and/or other materials provided with the distribution.
* Neither the name of Intel nor the names of its suppliers may be used to endorse or promote products derived from
  this Software without specific prior written permission.
* No reverse engineering, decompilation, or disassembly of the Software is permitted, nor any modification or
  alteration of the Software or its operation at any time, including during execution.

## SMAA (src/layer0_xenos/renderer/smaa/)

https://github.com/iryoku/smaa - SMAA.hlsl and its area / search tables, unmodified apart from two non-ASCII comment characters.


Copyright (C) 2013 Jorge Jimenez (jorge@iryoku.com)
Copyright (C) 2013 Jose I. Echevarria (joseignacioechevarria@gmail.com)
Copyright (C) 2013 Belen Masia (bmasia@unizar.es)
Copyright (C) 2013 Fernando Navarro (fernandn@microsoft.com)
Copyright (C) 2013 Diego Gutierrez (diegog@unizar.es)

Permission is hereby granted, free of charge, to any person obtaining a copy
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software. As clarification, there is no
requirement that the copyright notice and permission be included in binary
distributions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## stb_dxt (src/layer0_xenos/renderer/stb_dxt.h)

https://github.com/nothings/stb - DXT1/DXT5 compressor by Fabian "ryg" Giesen, ported to C by Sean Barrett. Public
domain (or MIT, at your choice); COGBox uses it under the public-domain option (Unlicense), unmodified. Used to re-encode a button-prompt sheet after COGBox's keyboard key caps
are drawn over it (GitHub #41).

## Native mouse look (ue3_mouse_look.h, mouse_look.cpp)

Copyright (c) 2026, Flippage and the gow3-recomp contributors
Copyright (c) 2026, Santiago Quiroz Upegui and the gow2-recomp contributors
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
