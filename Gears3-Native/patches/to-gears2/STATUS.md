# Patches for the shared layer 0 (Gears2-Native) - status

| Patch | What | Status |
|---|---|---|
| 0001-0003 | DLSS/FSR jitter and depth fixes | merged earlier |
| 0004 | shader warm-up / wait | merged e22f2ec |
| 0005 | colour transfers between 32bpp and 64bpp targets | merged e22f2ec, OFF (yellow-brick regression, notes/15) |
| 0006 | device-loss diagnostics (DRED) | merged e22f2ec |
| 0007 | export readback: keep the vertex mirror for pending pages (v2) | merged 9cebed9 |
| 0008 | texture gradient exponent bias | not merged (logging + switch) |
| 0011 | script pad sticks | WITHDRAWN - layer 0 already has LUP/LDOWN/LLEFT/LRIGHT and RUP/... tokens |
| 0012 | memexport census (logging) | merged 26ca070's parent (Gears 2, 2026-10-23 02:31) |
| 0013 | compare dump: raw ours / reference / last-writer images of compared colour resolves on chosen frames | merged 825aa07 (Gears 2 job 298) |
| 0014 | arm the CPU write watch on resolve pages (switch, use with watch_narrow); evidence [G] (correction 02:58) | merged 14da0b2 (default off) |
| 0015 | --gow2_renderer_log_vs=<VS hash>: log vertex bytes (as floats) + v2: viewport / scissor / alpha test of each draw of that VS (diagnostic, off) | merged faaf1f8 (v2) |
| 0016 | vs-trace: for each --gow2_renderer_log_vs draw, the colour-target decision (EDRAM mode, PS, targets, key, drawable) and where the draw ends (DRAWN or which skip) (diagnostic, off) | merged e38f63e (Gears 2 job 307b) |
| 0017 | export copy check: compare our memexport copies with their source when the readback lands (diagnostic, off) | drafted, not sent |
| 0018 | CPU-path texture loads read GPU-current pages from the mirror first (the Canary way; default ON, Shagg 07:18) | WITHDRAWN 07:35 - Gears 2 ports Canary's readback_resolve instead (same purpose, Canary's form) |
