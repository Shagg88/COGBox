# Third-party notices

COGBox's own code is under the MIT licence (`LICENSE`). The third-party code in this repository, and the libraries the
release download ships, are listed with their licence texts in:

- [`Gears2-Native/THIRD_PARTY_NOTICES.md`](Gears2-Native/THIRD_PARTY_NOTICES.md) - code used by all four ports (Xenia /
  Canary / Edge, gow2-recomp / gow3-recomp mouse look, SMAA, XeGTAO, FidelityFX CACAO, modcore, ...), including the
  parts in `Gears1-Native`, `Gears3-Native` and `GearsJ-Native`;
- [`Gears2-Native/cogbox/licenses/`](Gears2-Native/cogbox/licenses/) - the launcher's libraries and every SDK or DLL in
  the release download;
- the licence files inside each `third_party/` folder (Dear ImGui, miniz, stb, ...).

Gears 2 and Gears 3 native HDR use PsychoV35 by Carlos Lopez (ShortFuse / clshortfuse), with the self-contained edition
modifications by Musa Haji (mqhaji). Copyright and MIT permission/disclaimer text are retained in the embedded shader,
the [source notice](Gears2-Native/src/layer0_xenos/renderer/psychov35/LICENSE.txt), and the
[distribution notice](Gears2-Native/cogbox/licenses/PsychoV35-LICENSE.txt).

The UE3 HDR grading bridge also adapts RenoDX's Oklab conversion matrices,
both games' smoothed-power HDR boost from the Dishonored sample, and
uses its UE3 compress/sample/reconstruct strategy as a reference. Credit:
Carlos Lopez Jr. / ShortFuse and the RenoDX contributors. The
[RenoDX MIT notice](Gears2-Native/cogbox/licenses/RenoDX-Grading-LICENSE.txt)
ships beside both game executables. Reference files and the integration are
documented in [the UE3 grading notes](docs/ue3-hdr-grading.md).
