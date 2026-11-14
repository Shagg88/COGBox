# Gears 3 shader dump: HDR fallback audit

## Finding

The reported whole-screen brightness drop has a concrete mechanism in the
renderer: a writer after the native scene can invalidate the entire native
frame. Presentation then uses the SDR reference image at reference white.
This affects the whole scene even when that writer draws only a small UI
element. It is distinct from bounding just the UI material's emitted RGB.

The supplied dump contains **3,973 binaries**: 2,711 PS 5.1, 1,227 VS 5.1,
19 CS 6.6, 11 PS 6.6 and 5 VS 6.6. Regenerating recorded microcode using the
existing translator matched **3,914 files byte for byte**: 2,689 pixel and
1,225 vertex shaders. These matches identify the exact guest shader hashes,
microcode lengths and fingerprints, rather than guessing from filenames.
The remaining 59 entries are listed separately; they include native replay
shaders and renderer built-ins.

The evidence below combines that static scan with the Gears 3 session log
`Gears2-Native/cogbox/build/Gears3/logs/20261113-200548/gow3.log`.
Each row is a logged reason for native HDR fallback. A shader's appearance
in that log does not identify the exact on-screen effect or prove that every
use of that shader samples UI-only textures.

## Confirmed unclassified later writers

All four are pixel shaders, write color target 0, and passed the renderer's
current state/target eligibility checks when the fallback was logged.
This table records the initial failures; the live follow-ups below describe
the two profiles subsequently implemented.
`Canvas false` means neither a known UI pixel profile nor the accepted
Canvas-vertex/straight-alpha combination selected their float replay.

| Dump / DevKit PS hash | Guest shader hash | Code behavior | Logged result |
|---|---|---|---|
| `0xD782C90B` | `2C1E4E099E31E037` | Blends two 2D samples, applies a color scale/offset, saturates RGB, applies a power curve and output gain. This may be UI/video compositing or a scene effect; bound textures determine which. | Canvas false; supported true; entire native frame invalidated |
| `0xAFF5BC66` | `1C0932D0C22ECC2A` | One 2D sample plus a color offset, multiplied by the square of interpolated alpha. No explicit RGB saturation in its microcode. | Canvas false; supported true; entire native frame invalidated |
| `0xAFF65D3D` | `02B9355F857C5ED1` | Closely related one-texture material with one additional scalar color offset. No explicit RGB saturation in its microcode. | Canvas false; supported true; entire native frame invalidated |
| `0xDED6FC67` | `A1960D34295C6114` | Three 2D textures, clipping/discard tests, color transform and a combined alpha mask. Its structure resembles the already recognized clipped/masked UI materials. UI identity is an inference until its bindings are captured. | Canvas false; supported true; entire native frame invalidated |

Identity guards for a future implementation:

| Guest shader hash | Microcode dwords | FNV-1a fingerprint |
|---|---:|---|
| `2C1E4E099E31E037` | 51 | `B241E3475A37FF9C` |
| `1C0932D0C22ECC2A` | 21 | `88FB1A623EDF043F` |
| `02B9355F857C5ED1` | 24 | `3D1622CB63A21E75` |
| `A1960D34295C6114` | 135 | `D2EDE72A3297542B` |

The two short one-texture shaders demonstrate why searching only for
`saturate` misses this problem: their *fallback*, rather than an explicit
RGB clamp in the shader, discards the scene's HDR range.

## Separate menu render-state failures

| Dump / DevKit PS hash | Guest shader hash | Existing handling | Logged result |
|---|---|---|---|
| `0x2F8879EC` | `A7AB2D75C169DFDE` | Known menu color UI profile, 39 dwords | Canvas true; supported false |
| `0x69CB31E3` | `19055C4D86C6C08C` | Known menu atlas UI profile, 54 dwords | Canvas true; supported false |
| `0x5D2CB68B` | `A4A965C189287B99` | Unprofiled vertex-color passthrough, no textures, 9 dwords; fingerprint `A68F00446E79F015` | Canvas false; supported false |

Adding duplicate profiles for the first two will not address their logged
failures. `supported false` is a combined state test; this log does not name
the failing condition. Current eligibility requires one color attachment,
one sample, no enabled depth/stencil, no memexport, available bytecode and
the expected final R8 target/base/pitch. The affected draw's actual state
must determine the fix.

If stencil clipping is involved, the HDR replay must preserve that clipping
without applying stencil writes twice. Simply dropping the stencil/depth
checks would change menu coverage and can introduce artifacts.

## LUTs and other static candidates

The matched originals contain one shader with a 3D texture fetch:
`0x287C461B` / guest `2F284B68A112ED3E`, the already recognized Gears 3
scene material. It samples its 3D LUT at guest texture slot 3. That shader
can use different bound LUT contents across levels and grade changes; the
HDR implementation reads the current LUT instead of embedding one chart.
This scan does not establish that every possible final-post-process
permutation has been exercised or dumped.

The scan also flags **362 small 2D materials** and **22 small untextured
materials** as candidates. The heuristic requires a single color output,
no depth/memexport, small microcode, and limited 2D sampling without a
normal-lighting dot product. It deliberately labels candidates rather than
claiming they are all UI. World particles, additive lights, decals and
scene-texture effects can have similarly short programs. Several already
receive bounded replay through the existing Canvas vertex recognition.

## Correct handling and remaining evidence

For the four unclassified writers, capture the affected draw's PS/VS pair,
textures, blend factors, target, depth/stencil and ordering. The intended
handling depends on those bindings:

- **UI emission:** replay into the existing native FP16 target with bounded
  source RGB/alpha and the independent UI-white control, preserving coverage
  and blending over the HDR scene.
- **Multiplicative UI:** retain its bounded modulation factor without adding
  UI-white gain.
- **Scene-texture post-process:** supply the corresponding native scene
  image and preserve its HDR response. A UI-white clamp on the sampled world
  would reproduce the reported whole-scene problem. The two-sample gamma
  material additionally has an explicit pre-power saturation to account for.
- **Unsupported clipping/state:** preserve the actual coverage/state in the
  float replay; a pixel profile alone is insufficient.

The DevKit reported no available connection during the initial static scan, so those
affected bindings could not be captured. Static bytecode alone contains no
per-draw resource contents, blend state, depth/stencil state or frame order.
The logged hash identities and fallback causes are confirmed; exact visual
effect names and safe replay types initially remained to be established.
The live follow-up below confirms and handles one of those writers.

## Live follow-up and verified UI profile

After reconnecting to `renodx-devkit-mcp-12084`, a capture contained 1,827
draws. Draw 1812 uses PS `0xD782C90B` with VS `0x44CC706A` / guest
`5BF4E342D00F1593`. Both sampled textures are the same **160x80 BC3 UI
image**, rather than the world resolve. It uses straight-alpha blending
onto the expected final target. This confirms a UI-emission replay for
this captured use. The next draw has no float replay because this material
has already invalidated the native frame. Draw 1824's HDR encoder samples
`HDR reference-white fallback` at SRV 1, confirming the final range loss
in this frame. Details are saved in `work/g3-shader-audit/live-evidence.json`.
BC3 preview readback was unsupported; the texture identity/dimensions and
absence of a scene input come from the captured resource bindings.

The renderer now recognizes this pixel program with its verified dword
count/fingerprint, restricted to that vertex pair and straight-alpha
blending. The existing bounded linear UI shader output is replayed over
the retained HDR scene. Ordinary scene/material uses keep their original
program. The optional vertex restriction is part of the UI profile so the
same pixel hash cannot select this new replay in a different pipeline.

Fallback logging now includes the vertex hash, captured-pair check, color
count, samples, depth/stencil, memexport, bytecode availability, target
base/pitch/format and blend state. This makes the remaining combined
`supported false` failures diagnosable after restarting the rebuilt game.
Their state/coverage restrictions remain in place.

Both API checks accept the actual new bounded pixel shader and matching
captured vertex shader into an FP16 graphics pipeline. D3D12 and Vulkan UI
GPU readbacks also pass world-HDR preservation, bounded UI/partial coverage,
SDR chromaticity, no transient inversion, black, peak and negative-safe
gamma checks: **4/4 checks pass**. The normal Gears 3 EXE was rebuilt from
the existing game objects. These checks establish pipeline compatibility
and the existing UI-composition behavior; an in-game visual check after
installation is still required. The other logged writer profiles have not
been added without their affected bindings/state.

The tested build is installed at the existing
`Gears3-Native/build/Release-tu6/gow3.exe` path (78,846,464 bytes, SHA-256
`2C96B6DCF43418331FB75D018F6D9A2843E56A64D33D6FBAEC59D8F814B9DB48`).
The prior EXE was backed up by hash. Installed executable/runtime checks
passed; the normal Gears 2 executable is unchanged.

## Follow-up: destination-color modulation

The next affected capture, on `renodx-devkit-mcp-13272`, contained 1,470
draws. PS `0xAFF5BC66` / guest `1C0932D0C22ECC2A` appeared at draws
1445, 1447, 1451 and 1453, paired with VS `0x05F97824` / guest
`2E88B37100D9D0CE`. Each samples the same 68x76 BC1 texture. The RGB blend
is source times destination color, plus zero destination term; alpha is
retained. No resolved world image is sampled by this material. The log
explicitly shows this writer invalidating native HDR. Its bindings are
saved in `work/g3-shader-audit/modulation-evidence.json`.

This is a **modulation** profile, not emissive UI. It requires that captured
vertex pair and destination-color/zero/add blending. A dedicated native
output mode bounds the source before decoding into a linear factor; it
applies neither UI-white gain nor the scene highlight shoulder. That
factor then multiplies the existing FP16 scene. A neutral factor therefore
retains HDR values above one; darker factors apply the original darkening
without imposing a reference-white ceiling on the scene. This output mode
also corrects the existing recognized menu modulation profile.

Validation in `work/g3-shader-audit/modulation-checks.json` passes all
eight cases: real captured modulation PS/VS pipelines on D3D12/Vulkan,
numeric readback of the actual pixel shader over HDR backgrounds on both
APIs (24 draws each), the previous two-texture UI pair on both APIs and
both UI-composition readback suites. Numeric cases cover negative, zero,
half, neutral, above-one and FP16-range factors, with two peak values and
two UI-white values, checking retained alpha and the expected linear
modulation. Both scene tonemapper HLSL exports remain byte-identical.

The updated normal G3 EXE is installed at 78,846,976 bytes, SHA-256
`6E29F5A6FBE6CFB94B8023856AEE0819CD707466F0A253823B302E27C0D90885`.
Installed executable/runtime verification passed; the next live visual
check is separate from those GPU tests. The two remaining unclassified pixel writers are
`02B9355F857C5ED1` and `A1960D34295C6114`. The new diagnostic log confirms
that the three menu state failures involve enabled stencil; handling them
must retain the actual clipping behavior.

## Reproducible outputs

All generated game data stays in the local work directory:

- `work/g3-shader-audit/inventory.csv`: all 3,914 exact original matches,
  stages, fingerprints, sampling metadata and source paths.
- `work/g3-shader-audit/candidates.csv`: candidate and existing-profile
  subset, including every logged fallback shader.
- `work/g3-shader-audit/summary.json`: totals, log evidence and the 59
  unmatched binary entries with SHA-256 hashes.
- `work/g3-shader-audit/mapped/ps-<guest>.hlsl`: full regenerated HLSL for
  each pixel shader, alongside microcode disassembly and original bytecode.
- `work/g3-shader-audit/audit_dump.py`: inventory/report generator. It
  verifies full byte equality against the supplied dump before identifying
  a match.

The generated HLSL retains translator helper names and register operations;
it is an analysis export, not a hand-written replacement to inject. The
tables above describe the relevant operations in readable terms.
