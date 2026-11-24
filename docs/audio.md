# Audio settings (all four games)

Shagg, 23 Nov 2026: an AUDIO section on the launcher's Gameplay tab (no Audio tab). No 7.1, no Night Mode, no device
picker.

## Sound output (`--audio_channels`, SDK patch 126 - PR #441; replaces patch 125)

One value for all games (Shared). The shared SDL3 driver always mixes 5.1; the switch picks what reaches the device:

| Launcher | Switch | What the driver does |
|---|---|---|
| Auto (by your speakers) | (nothing sent) = `auto` | as before: 5.1 on a surround device, the stereo fold on 1-2 channels |
| Mono | `1` | the stereo fold, then (L+R)/2 at -3 dB on both speakers |
| Stereo | `2` | always the stereo fold |
| 5.1 surround | `6` | always 5.1 (Windows / SDL mix it down for fewer speakers) |

Log: `audio endpoint '...': N ch ...; submitting N ch`, then `audio_channels: <mode> -> <layout>`.
Greyed until the runtime has the switch (its description "Sound output layout").

## The game's own audio menu

Each game's Options > Audio has three sliders: Music, Effects, Dialogue. The launcher shows them as
"Game's own" (sends nothing) or 0..10, kept per game.

| Game | Menu sliders | Stored as | Profile id |
|---|---|---|---|
| Gears 1 | Music volume, Effects volume, Dialogue volume [S] | `WarProfileSettings` consts `MusicVolume`, `FxVolume`, `DialogueVolume`; `MAX_VOLUME_VALUE`, `GetDialogVolume` int -> float [S: WarGame export list] | not known |
| Gears 2 | Music, Effects, Dialogue [G: same UE3 profile class line as Gears 3] | `GearProfileSettings` ints [G] | not known |
| Gears 3 | MUSIC VOLUME, EFFECTS VOLUME, CHARACTER DIALOGUE VOLUME, plus BACK / DEFAULTS [S: the game's Coalesced menu text] | `GearProfileSettings` ints [G] | not known |
| Judgment | Music, Effects, Dialogue [G] | `GearProfileSettings` ints [G] | not known |

Range 0..10 and the defaults are guesses [G]; the test below settles them.

Game side: one layer 2 copy, `ue3/audio_menu.h`, from each game's existing profile getter hook. Switches
`--<game>_audio_music / _effects / _dialogue` (-1 = untouched) and `--<game>_audio_menu_ids=<m>,<e>,<d>`. Each game's
id table is still -1, so nothing is written until the ids are known; the launcher greys the sliders
("the game's slider ids are not found yet") until `kAudioMenuIdsKnown` in `gears_settings.cpp` is set for that game.

## Which value wins

**The last change wins, wherever it was made.**

1. "Game's own" in the launcher: the launcher never sends that slider; the game keeps its own value.
2. A value set in the launcher is written into the profile when the game first reads that slider. The game saves it
   with its next profile save.
3. The game logs every slider value it reads, once per change: `<game>_audio_menu: now <key>=<v> (id N)`.
4. When the game closes, the launcher reads the last such line per slider. If it differs from the launcher's value,
   the player changed it in the game's menu: the launcher takes the game's value and saves it. The next start sends
   the value the game already has, so nothing is overwritten.
5. A slider changed in the game's menu during play is also what is written again if the game reloads its profile.
6. No line for a slider (the game closed before reading it): the launcher keeps its own value.

Known gap: the game's menu value reaches the launcher only through the log of a run that closed. A crash before the
log is written loses that run's menu change (the launcher's value is sent again next time).

## Test job (written, not queued)

Proven route: title screen, Options > Audio (no gameplay needed for the menu itself), then one chapter from
`docs/test-routes.md` with `--cogbox_test_state=true` for the sound layout.

1. Find the ids (no game start): run `GearsJ-Native/tools/gj100_profile_ids.ps1`'s method on each game's
   GearGame.xxx / WarGame.xxx and list the ProfileMappings ids named MusicVolume / FxVolume (EffectsVolume,
   SoundEffectsVolume) / DialogueVolume (DialogVolume). Fallback: `--<game>_profile_log=true`, open Options > Audio,
   move each slider one step, read `get int id=...` lines whose value changes.
2. Per game, with the ids: `--<game>_audio_menu_ids=<m>,<e>,<d> --<game>_audio_music=3 --<game>_audio_effects=7
   --<game>_audio_dialogue=10 --audio_channels=1`. Log must show:
   - `<game>_audio_menu: ids music=<m> effects=<e> dialogue=<d>; launcher values music=3 effects=7 dialogue=10`
   - `<game>_audio_menu: settings array at object+0x...` and `<game>_audio_menu: id <m> 0x... -> 0x00000003` (x3)
   - `<game>_audio_menu: now music=3 (id <m>)`, `now effects=7`, `now dialogue=10`
   - `audio_channels: 1 -> mono (stereo fold, L/R averaged, -3 dB)`
   - Screenshot of Options > Audio showing 3 / 7 / 10 (the game's own scale - note the real max).
3. Same run: move Music to another step in the menu, back out (profile save), quit. Log: `now music=<new>`.
   Start again without `_audio_music`: the menu shows the new value (it was saved).
4. `--audio_channels=2` and `=6` on a 5.1 or stereo device: `audio_channels: 2 -> stereo fold`, `6 -> 5.1`,
   and the `submitting N ch` line.
5. Every run carries the full-diagnostics run beside it (test rules).
