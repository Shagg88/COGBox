# COGBox achievements (PR #187, Flippage)

Global achievements for all four games: the unlocks are cached in `<COGBox>\achievements`, an unlock shows a popup
(Xbox 360, Xbox or PlayStation style, with `assets/sounds/achievement.wav`), F7 / the in-game menu open an Achievements
page, and the launcher has an ACHIEVEMENTS tab (popup settings, then every game's list). Code:
`Gears2-Native/src/layer0_xenos/achievement*.cpp`, `popup_*.cpp`, `cogbox/src/achievements_tab*.cpp`; tests in
`Gears2-Native/tests/layer0_achievements_*` and `cogbox/test/achievements_tab_test.cpp`. The full design and plan notes
are in PR #187.

Known follow-ups (none affects normal play): two simultaneous game-completing unlocks can queue two Platinum popups;
the font atlas is ~32 MB; a failed icon load stays "?"; sorted by name, a hidden secret sits at its real
name's place; F7 does nothing with `cogbox_menu_enabled=false`.

Testing: `--cogbox_ach_test_keys=true` (off by default) makes F3 show a sample popup and F4 a sample Platinum. F3 / F4
are free in all four games (F1 menu, F2 / F5 / F6 / F8-F11 AO debug keys and Gears 3's cheat keys, F7 the
Achievements page, F9 the upscaler capture key, Backslash the screenshot).
