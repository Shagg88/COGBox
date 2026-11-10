# tools\analysis - Gears 3's code-matching and image tools (reference copies)

Our own scripts (MIT). They read files you make yourself from your own disc. No game data is in this folder.
Labels: [V] verified on Gears 2 -> Gears 3, [G] untested elsewhere.

| Script | What it does |
|---|---|
| `match.py <g2addr>...` | Finds the Gears 3 function most like a Gears 2 one (same UE3 code, new addresses). Disassembles both with capstone (PPC32 big-endian), masks registers/immediates, diff-ratio ranks candidates, and reports the offset of the given address inside the match. [V] |
| `side.py <g2addr> <g3func> [window]` | Prints the two matched functions side by side around a site, so you can check a hook site by eye. [V] |
| `xref.py <addr>` | Finds code that references a global (`lis rX,hi` + `d(rX)` / `addi rX,lo` within a short window). [V] |
| `funcs.py <generated dir>` | Function list (start, end) from rexglue's generated code (`DEFINE_REX_FUNC`; end = next start, approximate). |
| `addrmap.py <generated dir> <out.tsv>` | Address -> instruction -> function map from the `// <asm>` comments rexglue writes in the generated code. |
| `extract_shaders.py` | Pulls Xbox 360 shader containers (flags 0x102A1100 / 0x102A1101) out of any file into `<vs|ps>-<XXH3>.ucode`. Output is game-derived: keep it out of git. |
| `xex_addr_map.py` | Where code addresses moved between two builds of the same game (e.g. base disc -> title update). Game-neutral. |

**Paths:** `match.py`, `side.py` and `xref.py` read `/root/g3tools/` (g2image.bin, g3image.bin, *funcs.json) - the
decrypted images and the function lists made with `funcs.py`. Change `T` / the `sys.path` line for your own folder.
Needs: Python 3, `capstone`, `xxhash` (extract_shaders only).

**Notes from use:**
- Masked windows work well for XDK / D3D / UE3 engine code. Game code and code that LTCG inlined differently match
  poorly, so check every match with `side.py` before hooking it.
- A match tells you where; the hook still needs the site read by eye: which register, before or after the instruction,
  and whether it's a branch target.
