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
