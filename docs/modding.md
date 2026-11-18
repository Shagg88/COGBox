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
Program files are never used from a mod: programs and libraries, scripts, installers, shortcuts and other things
Windows runs, and archives inside the mod are left out when it is added. Folders inside a mod that are links
(junctions or symlinks) are not followed: copy the files in (the log names any link it skipped).

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

## Texture packs (every game)

A texture pack replaces the game's textures with your own, usually bigger ones, without touching any package. It is a
mod like any other (a folder in `<Game>\mods\`, or a `.zip` added in the Mods tab), and it works in Gears 1, 2, 3 and
Judgment: the renderer they share does the replacing. The Mods tab shows how many textures a mod's pack has.

```
mods\MyTextures\
  0031EB540D50E47E_18_256x256.dds     a replacement: the 16-digit hash of the texture it replaces, then anything
  textures\...\*.dds                 any folders inside the mod are searched
```

- **The name** starts with the hash of the game texture it replaces (16 hex digits), then `_` or `.dds`. The hash is
  the one the ReXGlue-based ports use, so packs made for the Gears of War 2 PC port load unchanged: put the pack's
  folder (with its `4D53082D\replace` inside) in `Gears2\mods\`.
- **Size:** the original's, or the same whole multiple of it across and down (2x and 4x are usual; up to 8x).
- **Mip levels:** a texture the game draws with mip levels needs them in the file too, down to at least the game's
  smallest (save with every level: `texconv -m 0`).
- **Formats:** BC1 (DXT1), BC2, BC3 (DXT5), BC4, BC5, BC7, R8, R8G8 and R8G8B8A8 / B8G8R8A8, in a DDS file (DX10
  header or the classic one). sRGB files are read as plain ones (the game does its own gamma).
- **Which textures:** 2D textures. Cube maps, volume textures, render targets (screens the game draws into) and
  textures the game reads as signed (some normal maps) keep the game's own.
- **Largest size:** each mip level must be 64 MB or less (for example R8G8B8A8 up to 4096x4096, BC7 or BC3 up to
  8192x8192). A bigger file is left out and the log says why: save it as BC7 / BC3 or at a smaller scale.
- **Known limit - textures the game reads by texel:** a few textures are read by texel position rather than stretched
  over a surface (look-up tables, some fonts and UI pieces). A bigger replacement of one of those is read at the
  wrong place, so it shows only part of the image. Keep such textures at the original size (1x).
- **Order:** where two packs replace the same texture, the mod lower in the list wins, as with files.
- **Folders left out:** `dump`, `.build` and `replace_<name>` folders inside a mod are not read.
- Bigger textures take more video memory: a 4x pack has 16 times the pixels.

**Making a pack.** Start the game with `--gow2_texture_pack_dump_dir=<folder>` (a line in `extra-args.txt` next to
`CogBox.exe`) and play the parts you want: every texture the game draws is written there once, as
`<hash>_<format>_<width>x<height>.dds`, already named for a pack. Edit or upscale the images, save them as DDS with
their mip levels, keep the hash at the start of the name, and put them in a mod. The game's log says how many
textures each pack gives (`gow2_texture_pack:`), and names any file it could not use and why.

Textures made from the game's art are derived from it: whether you may share them is a question of its owner's rules,
as for any mod that carries the game's files.

## Packaging and publishing your mod

COGBox's **FABRICATOR** screen (the launcher's FABRICATOR button, or **More in FABRICATOR** on a game's Mods tab) has a
**CREATOR** tab that checks your mod's folder and makes the zip. It works offline.

1. **Choose your mod's folder** (or drop it on the window): the folder laid out as in "The folder" above, with its
   `mod.ini`.
2. **Fill in mod.ini.** The form edits these `[Mod]` keys and nothing else; other sections, keys and comments in your
   `mod.ini` stay as they are. **Save mod.ini** writes it.

   | Key | What to put |
   |---|---|
   | `Name` | The mod's name players see. An update of an already added mod is matched by this name (case is ignored). |
   | `Game` | `gears1`, `gears2`, `gears3` or `judgment`. |
   | `Version` | Free text, such as `1.0`. |
   | `Author` | Your name. |
   | `Description` | What it does. |
   | `Side` | `everyone`, `host` or `client` (see above). |
   | `Requires` | Mods that must be on too, by name, comma-separated: `Other Mod, Another Mod`. The game leaves your mod out without them. |

3. **Fix what the check lists.** The check looks for what a clean package needs: a `mod.ini` with `Name` and `Game`
   (not for a newer mod API), safe file names (no unsafe or duplicate names, no links), and the limits (50 000 files,
   8 GiB unpacked, a `mod.ini` of at most 256 KB; the 4 GiB zip limit is checked when the zip is made). It also
   lists **program files**, because a clean package has none: programs and libraries (`.exe`, `.dll`, `.xex`), scripts (`.bat`, `.cmd`, `.ps1`, `.vbs`, `.js`), installers, shortcuts, registry files and other
   things Windows runs, and archives (`.zip`, `.7z`, `.rar`, `.cab`) inside the mod. When a player adds a mod that has
   such files, they are left out and the install says so. `fabricator.json` is never packed, and a zip's own is ignored.
4. **Make a zip.** Pick where to save it; its name is the mod's name and version. Test it by adding it to your own
   LIBRARY.
5. **Publish it** where you like. COGBox has no mod hosting. A common place is Nexus Mods (nexusmods.com), which COGBox
   plans to browse later:
   1. Make a mod page for your game there.
   2. Upload the zip as the main file.
   3. Add the **COGBox** tag.
   4. Say which COGBox version it needs (the CREATOR tab shows the version you have).

Don't put the game's own files, or files made from them, in what you share (see "The folder" above). Using Nexus Mods
doesn't mean Nexus Mods supports or endorses COGBox, and its own rules apply to your page.

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
