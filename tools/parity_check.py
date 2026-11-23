#!/usr/bin/env python3
"""Four-game parity check (Shagg, 23 Nov: "if one game has it, all of them should have it if it applies").

Lists every per-game option (layer 3 cvars, gow1_/gow2_/gow2_patch_/gow3_/gowj_ prefixes stripped) and every per-game
hook (manifest midasm hooks, Gow1/Gow2/Gow3/Gowj prefixes stripped) that is NOT in all four games, then checks each
against docs/parity.md:
  - listed under "Only where it applies" (with a reason)  -> fine
  - listed under "Gaps to close"                         -> reported as an open gap
  - not listed anywhere                                  -> reported as UNREVIEWED (a new one-game feature)
Exit code 1 when anything is unreviewed, so a build or test job can refuse to pass.

Usage: python3 tools/parity_check.py [repo root]
"""
import glob
import os
import re
import sys
import tomllib

GAMES = {"Gears 1": ("Gears1-Native", "gow1_manifest.toml"), "Gears 2": ("Gears2-Native", "gow2_manifest_tu6.toml"),
         "Gears 3": ("Gears3-Native", "gow3_manifest_tu6.toml"), "Judgment": ("GearsJ-Native", "gowj_manifest.toml")}


def collect(root):
    found = {}  # name -> set of games
    for game, (folder, manifest) in GAMES.items():
        for f in glob.glob(os.path.join(root, folder, "src", "layer3_*", "**", "*.cpp"), recursive=True):
            with open(f, encoding="utf-8", errors="replace") as fh:
                for m in re.finditer(r"REXCVAR_DEFINE_\w+\((\w+)", fh.read()):
                    name = "option " + re.sub(r"^(gow1|gow2|gow3|gowj)_(patch_)?", "", m.group(1))
                    found.setdefault(name, set()).add(game)
        with open(os.path.join(root, folder, manifest), "rb") as fh:
            hooks = tomllib.load(fh).get("entrypoint", {}).get("midasm_hook", [])
        for h in hooks:
            name = "hook " + re.sub(r"^(Gow1|Gow2|Gow3|Gowj|GowJ)(Patch)?", "", h["name"])
            found.setdefault(name, set()).add(game)
    return found


def listed(root):
    path = os.path.join(root, "docs", "parity.md")
    section, out = None, {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("## "):
                section = line[3:].strip()
            m = re.match(r"\|\s*`([^`]+)`", line)
            if m and section:
                out[m.group(1)] = section
    return out


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    found, doc = collect(root), listed(root)
    gaps, unreviewed = [], []
    for name, games in sorted(found.items()):
        if len(games) == len(GAMES):
            continue
        where = ", ".join(g for g in GAMES if g in games)
        sec = doc.get(name)
        if sec is None:
            unreviewed.append(f"{name} ({where})")
        elif sec.startswith("Gaps"):
            gaps.append(f"{name} ({where})")
    print(f"parity: {len(found)} per-game options and hooks, {sum(1 for g in found.values() if len(g) < len(GAMES))} "
          f"not in all four games; {len(gaps)} open gaps, {len(unreviewed)} unreviewed")
    for g in gaps:
        print("  GAP:", g)
    for u in unreviewed:
        print("  UNREVIEWED:", u)
    return 1 if unreviewed else 0


if __name__ == "__main__":
    sys.exit(main())
