# Making mods for COGBox (Gears of War 3)

A mod is a folder in `Gears3\mods\`, ticked in COGBox's **Mods** tab. The game sees the mods' files in place of, or
next to, its own; your game folder is never changed. Mods are data (packages, maps, config, movies): the game's script
is cooked, so new logic comes from COGBox itself, through the services below.

Never put the game's own files, or files made from them, in a mod you share: players bring their own game.

## The folder

```
mods\MyMod\
  mod.ini                         optional: name, rules, campaigns (below)
  GearGame\CookedXbox360\...      files at the paths the game uses: they replace the game's, or are added
  config\*.ini                    config fragments: changes merged into the game's settings
```

Mods made for the Xbox 360 are often shared as bare files. A package (`.xxx`, `.upk`), texture cache (`.tfc`) or
`Coalesced_*.bin` at the mod's top goes to `GearGame\CookedXbox360`, and a movie (`.bik`) to `GearGame\Movies`.
Programs (`.xex`, `.dll`, `.exe`) are never used from a mod. Folders inside a mod that are links (junctions or
symlinks) are not followed: copy the files in (the log names any link it skipped).

**Load order:** the Mods tab's order, top to bottom. Where two mods have the same file, the lower one wins.

## mod.ini

```ini
[Mod]
Name = My Mod
Version = 1.0
Author = You
Description = What it does (shown when you point at it in the Mods tab)
Api = 1                       ; the mod API it is made for (1: campaigns). Leave it out for a mod without them
Requires = Other Mod          ; mods that must be on too (name or folder, comma-separated), else this one is left out
Game = gears3                 ; the game it is for (the default)
Enabled = true                ; false: off, without removing it from the list
Side = everyone               ; who needs it in co-op / System Link (below)
Content = MPCH_COGMarcusG2MP_SF   ; the game's own content packages your levels use that it would not load there
```

Keys are case-blind. A mod without a `mod.ini` still works, named after its folder.

**Side** says who needs the mod in co-op and System Link:

| Side | Meaning | Compared when a player joins? |
|---|---|---|
| `everyone` (default) | Anything both ends simulate: packages, maps, weapon stats | Yes: every player needs the same `everyone` mods |
| `host` | Rules the host's game applies (game-mode settings) | No |
| `client` | Only what this player sees or hears (menus, HUD, text) | No |

## Config fragments (`config\*.ini`)

Fragments are merged, in load order, into the game's packed settings (every `Coalesced_*.bin`), so several mods can
change settings without replacing whole files.

```ini
##### Xbox360-GearEngine.ini    ; the packed file whose name ends with this
[SystemSettings]
MotionBlur=False                 ; Key=v   replaces every value of Key
+Suppress=DevLoad                ; +Key=v  adds v unless it is there
-Suppress=Init                   ; -Key=v  removes v
*Entries=(Identifier=X,Name=Y)   ; *Key=v  replaces the value with the same first field (a menu entry), else adds it
```

Sections and files that do not exist yet are created.

## Campaigns (mod API 1)

A mod adds campaign chapters by declaring them. **Never write act or chapter numbers yourself:** COGBox gives them
out, so mods never collide with each other, with the game, or with RAAM's Shadow.

```ini
[Mod]
Name = My Campaign: Chapter 1
Api = 1

[Campaign]
Id = mycampaign               ; stable id (lower case): chapters in other mods join the campaign by it
Title = My Campaign           ; the act's name in Select Chapter
Order = 50                    ; where it sorts among campaigns (default 100)
Screenshot = UI_Portraits_G3.SP.T_Portraits_SP_A1C0_Act1
SquadCatchUp = 800            ; optional adapter (below)
MaxPlayers = 2                ; optional: co-op players its chapters take (1-4; the game's 4 when not given)

[Chapter.1]                   ; its place in the campaign: shown as "Chapter 1"
Map = MyCampaign_Ch1_P        ; the chapter's persistent map (in your mod)
Title = The First One
Location = Somewhere
Time = Emergence Day + 1 year
Screenshot = ...              ; optional: the campaign's otherwise
ContentPackages = MPCH_COGDomG2MP_SF   ; the game's own packages the chapter needs (as Content above)
```

How the slots are given out:
- **Acts:** campaigns get the act numbers after every act the game already has (its five, RAAM's Shadow's, and those
  of older mods that write their own). They are taken in this order:
  1. `Order`;
  2. then the load order of each campaign's first mod;
  3. then `Id`.
- **Chapters:** each chapter takes the next free chapter number from 32 to 49, in `[Chapter.N]` order.
- **Joining:** chapters of one campaign may come from several mods, each with the same `Id`. A trilogy, for example,
  can ship one chapter per mod.
- **Same mods, same slots:** the same mods in the same order always get the same slots, so progress and checkpoints
  stay valid. Adding or removing a campaign that sorts before yours moves yours to other slots.

**Checking your mod:** the Mods tab shows where each chapter landed ("Select Chapter: Act VI My Campaign, chapter 1")
and any `mod.ini` key the game does not understand. The game's log (`Gears3\logs\<session>\gow3.log`) lists every
decision on the `mod_api:` and `ue3_mods:` lines.

**Engine-difference adapters** (in `[Campaign]`, off unless given): behaviour a converted campaign needs that the game
does not do itself.

- `SquadCatchUp = <distance>`: a squad AI that is stuck farther than this from player 1 is brought back behind them. It
  must have stood still for 6 s and its last path search must have failed. Converted levels can leave an AI's path
  search failing (the trilogy's Dom on the prison's hard route), while their scripting still waits for it. An AI that
  is moving, or waiting where the level's scripting put it, is never moved. `--mod_squad_log` logs what it sees every
  10 s.

**Co-op players:** `MaxPlayers = <1-4>` in `[Campaign]`. The game takes four players in a campaign, one for each
squad slot. A campaign with fewer playable squad members (Gears 1 and 2 have two: Marcus and Dom) declares how many.
While one of its chapters is loaded, a further player cannot join the game. Any of the campaign's mods may give it;
the smallest number wins. The Mods tab shows it on the campaign's line.

**Class defaults:** a `[ClassDefaults]` section changes a class's default values while one of the campaign's chapters
is loaded, and puts them back when another map loads:

```ini
[ClassDefaults]
GearPawn_SomeClass.bSomeFlag = false     ; bool (true / false), byte (0-255), int, or float (with a '.')
GearPawn_SomeClass.GroundSpeed = 450.0
```

Actors copy their class's defaults when they spawn, so the next spawn has the new value (actors already alive get it
too). The log has a `mod_api: class defaults:` line for each one, and says when a class or property is not found.
`--mod_log_props` (below) shows what a live actor ends up with.

Older mods that write their own `GearCampaignActData` / `GearCampaignChapterData` fragments still work. Their slots
are kept free for them.

## What the game checks, and where to see it

| Problem | What happens |
|---|---|
| A required mod is missing or off | The mod is left out: the Mods tab and the log say why |
| The mod is made for another game | Left out |
| A `mod.ini` key in `[Campaign]` / `[Chapter.N]` that is not one of the above | Ignored, and shown |
| A chapter with no `Map`, or not in a campaign | That chapter is left out, and shown |
| More chapters than free slots (49 is the last) | The extra chapters are left out, and shown |
| Two mods give a campaign the same chapter number | The later mod's chapter is left out, and shown |
| `Api` higher than this COGBox knows | Loaded, with a note to update COGBox |

## Testing a mod (developer switches)

These are command-line switches for the game (add them in the launcher's extra arguments). They change nothing unless
they are given.

- **Start straight in a chapter:**
  `--ue3_exec_at="1500:open GearGame_P?game=geargamecontent.GearGameSP?chapter=32"`
  - The chapter number is the slot the Mods tab shows ("Select Chapter: ...", or the game's log: `mod_api: campaign ...`).
  - 1500 is a game tick, about 25 s into the title screen. A mod's content packages are loaded and kept by then; much
    earlier, the chapter can start before them.
  - `--ue3_launch=GearGame_P?chapter=N` (at start-up) does not work for mod chapters: the game looks the chapter's map
    up before COGBox can add it.
- **Play a test the same way every time: `--cogbox_input_script=<file>`.** A text file of timed controller events is
  played into player 1's controller, with no keyboard and no window focus needed. Each line starts with the seconds
  since the game started:

  ```
  30 pad START 200          # buttons held for ms: A B X Y LB RB LS RS BACK START UP DOWN LEFT RIGHT, '+' joins
  60 trig R 300             # a trigger
  70 stick L 0 1 2000       # a stick held (x, y from -1 to 1)
  90 pos                    # player 1's position in the log (to plan a route)
  95 walkto 138 1042        # walk there
  110 follow COGDom 60      # walk after a squad mate
  140 choose L              # a path choice: waits for it, then that side
  150 route -950,1150;-1210,1227 20
  400 quit
  ```

  Every verb is listed in `Gears3-Native/src/layer2_ue3/include/ue3/script_input.h`. The game's log has a
  `script_input:` line for each step.

  Gears 1 takes the same scripts (`Gears1-Native/src/layer2_ue3/include/ue3/script_input.h`). Its `choose` presses
  the side's trigger at once, so time it to when the choice is up. Gears 1 and a converted Gears 1 level share map
  coordinates, so one script's `teleport` and `face` lines give the same view in both: a side-by-side comparison.
- **See what a live actor holds: `--mod_log_props="Class:Prop,Prop.Sub;Class2:Prop"`.** Every 5 s, each live instance
  of each class logs those properties. `Prop.Sub` follows an object reference one step:

  ```
  props: GearPawn_COGMarcusG1_0 Mesh.AnimRotationOnly=1
  ```
- **See where a level's scripting stops: `--mod_kismet_trace=<part of a package name>`** (`1`: every package). Each
  Kismet action, condition and event that runs is logged with its package and sequence path, sampled ten times a
  second:

  ```
  kismet: [GOW1_SP_Prison_S04_Scripting] G1_SP_Prison_S04_Scripting.towerFalls (Sequence) x1
  ```

  The last lines before a stall show the op the scripting waits behind: often a touch, a gate, or a spawner's
  "All Dead".
