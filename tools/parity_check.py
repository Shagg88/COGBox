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


def shared_macros(root):
    """23 Nov: options a game gets through a shared layer-2 macro (e.g. UE3_AUDIO_MENU_CVARS(gow1, ...) defines
    gow1_audio_music ...). Returns macro name -> option names (prefix stripped)."""
    out = {}
    for h in glob.glob(os.path.join(root, "Gears2-Native", "src", "layer2_ue3", "include", "**", "*.h"), recursive=True):
        with open(h, encoding="utf-8", errors="replace") as fh:
            lines = fh.read().split("\n")
        i = 0
        while i < len(lines):
            m = re.match(r"#define (\w+)\(\s*(\w+)", lines[i])
            if not m:
                i += 1
                continue
            name, p, body = m.group(1), m.group(2), [lines[i]]
            while body[-1].rstrip().endswith("\\") and i + 1 < len(lines):
                i += 1
                body.append(lines[i])
            opts = re.findall(r"REXCVAR_DEFINE_\w+\(\s*" + p + r"\s*##\s*_(\w+)", "\n".join(body))
            if opts:
                out[name] = opts
            i += 1
    return out


def collect(root):
    found = {}  # name -> set of games
    macros = shared_macros(root)
    for game, (folder, manifest) in GAMES.items():
        for f in glob.glob(os.path.join(root, folder, "src", "layer3_*", "**", "*.cpp"), recursive=True):
            with open(f, encoding="utf-8", errors="replace") as fh:
                text = fh.read()
                for m in re.finditer(r"REXCVAR_DEFINE_\w+\((\w+)", text):
                    name = "option " + re.sub(r"^(gow1|gow2|gow3|gowj)_(patch_)?", "", m.group(1))
                    found.setdefault(name, set()).add(game)
                for mac, opts in macros.items():   # 23 Nov: options from shared macros count too
                    if re.search(r"\b" + mac + r"\(\s*(gow1|gow2|gow3|gowj)\b", text):
                        for o in opts:
                            found.setdefault("option " + o, set()).add(game)
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


PREFIX = {"Gears 1": "gow1", "Gears 2": "gow2", "Gears 3": "gow3", "Judgment": "gowj"}


def all_cvars(root, folder):
    """Every cvar a game's exe has: its own sources plus the shared Gears2-Native layers 0-2 every game compiles."""
    names = set()
    paths = glob.glob(os.path.join(root, folder, "src", "**", "*.cpp"), recursive=True)
    for layer in ("layer0_xenos", "layer1_*", "layer2_ue3"):
        paths += glob.glob(os.path.join(root, "Gears2-Native", "src", layer, "**", "*.cpp"), recursive=True)
        paths += glob.glob(os.path.join(root, "Gears2-Native", "src", layer, "**", "*.h"), recursive=True)
    macros = shared_macros(root)
    for p in set(paths):
        with open(p, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
        names.update(re.findall(r"REXCVAR_DEFINE_\w+\((\w+)", text))
        for mac, opts in macros.items():
            for m in re.finditer(r"\b" + mac + r"\(\s*(gow1|gow2|gow3|gowj)\b", text):
                names.update(m.group(1) + "_" + o for o in opts)
    return names


def launcher_unknown(root):
    """23 Nov: switches the launcher writes as --gowN_x that the game's exe doesn't define (gow2_ renderer switches are
    shared: every game has them). Reported, not fatal - some are gated on the exe offering them."""
    path = os.path.join(root, "Gears2-Native", "cogbox", "src", "gears_settings.cpp")
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8", errors="replace") as fh:
        sent = set(re.findall(r'"--((gow1|gow2|gow3|gowj)_\w+)', fh.read()))
    have = {g: all_cvars(root, f) for g, (f, _) in GAMES.items()}
    out = []
    for full, pre in sorted(sent):
        games = [g for g, p in PREFIX.items() if p == pre] if pre != "gow2" else []
        if pre == "gow2":   # shared renderer prefix or Gears 2's own: fine if ANY game defines it, else unknown
            if not any(full in h for h in have.values()):
                out.append(f"--{full} (no game defines it)")
            continue
        for g in games:
            if full not in have[g]:
                out.append(f"--{full} -> {g} doesn't define it")
    return out


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    found, doc = collect(root), listed(root)
    gaps, unreviewed, on_hold = [], [], []
    for name, games in sorted(found.items()):
        if len(games) == len(GAMES):
            continue
        where = ", ".join(g for g in GAMES if g in games)
        sec = doc.get(name)
        if sec is None:
            unreviewed.append(f"{name} ({where})")
        elif sec.startswith("Gaps"):
            gaps.append(f"{name} ({where})")
        elif sec.startswith("On hold"):   # Shagg's call: reviewed, not an open gap
            on_hold.append(f"{name} ({where})")
    print(f"parity: {len(found)} per-game options and hooks, {sum(1 for g in found.values() if len(g) < len(GAMES))} "
          f"not in all four games; {len(gaps)} open gaps, {len(on_hold)} on hold, {len(unreviewed)} unreviewed")
    for g in gaps:
        print("  GAP:", g)
    for u in unreviewed:
        print("  UNREVIEWED:", u)
    for w in launcher_unknown(root):
        print("  LAUNCHER:", w)
    return 1 if unreviewed else 0


if __name__ == "__main__":
    sys.exit(main())
