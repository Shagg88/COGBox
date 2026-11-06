# COGBox achievements (PR #187, Flippage)

Global achievements for all four games: the unlocks are cached in `<COGBox>\achievements`, an unlock shows a popup
(Xbox 360, Xbox or PlayStation style, with `assets/sounds/achievement.wav`), F7 / the in-game menu open an Achievements
page, and the launcher has an ACHIEVEMENTS tab (popup settings, then every game's list). Code:
`Gears2-Native/src/layer0_xenos/achievement*.cpp`, `popup_*.cpp`, `cogbox/src/achievements_tab*.cpp`; tests in
`Gears2-Native/tests/layer0_achievements_*` and `cogbox/test/achievements_tab_test.cpp`. The full design and plan notes
are in PR #187.

Known follow-ups (none affects normal play): two simultaneous game-completing unlocks can queue two Platinum popups;
the popup cvars are written on the UI thread while a guest thread may read them; the PS5 popup runs off screen below
~440 px wide; the font atlas is ~32 MB; a failed icon load stays "?"; sorted by name, a hidden secret sits at its real
name's place; F7 does nothing with `cogbox_menu_enabled=false`.
