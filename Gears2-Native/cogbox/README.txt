COGBox - Gears of War native PC ports
=====================================

COGBox starts the native PC ports of Gears of War 2, Gears of War 3 and Gears of War: Judgment.
These are fan-made ports, rebuilt from the Xbox 360 games' own code by static recompilation. This
is an unofficial fan project, not made by or connected to Microsoft, Xbox Game Studios, The
Coalition or Epic Games.

NO GAME FILES ARE INCLUDED. You need your own copy of each game.


What you need
-------------
- Windows 10 or 11, 64-bit.
- A CPU with AVX2 (Intel Haswell / AMD Ryzen or newer, about 2013 on).
- A graphics card with Direct3D 12.
- Your own copy of the game, in one of these forms:
    * a disc image (.iso) of the US disc,
    * a Games on Demand copy from your console's hard drive, or
    * a folder with the disc's files (default.xex and the rest) that you already extracted.
- For Gears of War 2 and 3: the last title update (Title Update 6) is recommended - it is the
  default and the fastest build. Without it, COGBox starts the disc version; you can also pick
  the disc version on a game's Gameplay tab. Judgment's Title Update 4 is optional.
  The update is a file on a console that has played the game online, in
      Content\0000000000000000\<title ID>\000B0000\
  (Gears 2: 4D53082D, Gears 3: 4D5308AB, Judgment: 4D530A26).
- About 8 to 16 GB of free space per game if COGBox unpacks a disc image for you.


Getting started
---------------
1. Unzip the whole folder somewhere you can write to, for example C:\Games\COGBox. Program Files
   won't work, because Windows protects it.
2. Start CogBox.exe. The programs aren't code-signed, so the first time Windows may show "Windows
   protected your PC": choose "More info", then "Run anyway".
3. Turn to a game and press ADD YOUR COPY. Choose your disc image, Games on Demand file, or
   default.xex in an extracted folder. You can also drop it onto the COGBox window.
   COGBox works out which game it is, unpacks it next to itself if needed, and checks it is the
   release the ports are made for.
4. Add the title update the same way: ADD UPDATE, or "Add the title update" on the game's About tab.
5. Press PLAY. SETTINGS opens the game's page, which has graphics, frame rate, controls and more.
   Every setting has a short description.

Controller: the D-pad turns the carousel, LB / RB choose Play or Settings, A presses it,
X opens the settings, Y gets box art. On a settings page LB / RB change tab, D-pad down past
the last option reaches Library / Save / Play, and B jumps to Library.
Keyboard: the arrow keys turn it, Enter plays, Esc goes back.

In the game: Back + Start (or F1) opens the in-game menu - the same settings, Resume,
Restart to apply and Quit game. Most settings take effect when the game restarts; Restart to
apply does that for you.


Where things are
----------------
Everything stays inside the COGBox folder:
  COGBox.ini            what you added, play time, launcher choices
  settings.ini          each game's settings
  <Game>\game\          the files unpacked from your copy (not there if you pointed at your own folder)
  <Game>\user\          saves
  <Game>\logs\          one folder per play: the log, the arguments used, any crash report
  <Game>\shadercache\   shaders built while playing, kept so the next play doesn't stutter
  covers\               put your own box art here as gears2.png, gears3.png and judgment.png
                        (or .jpg). Without them, COGBox draws its own covers.

To update, use the UPDATE TO button on the main screen (or the one on any game's About tab), or unzip a new release over the folder. Your
games, saves and settings are not in the zip, so they are kept.


When something goes wrong
-------------------------
If a game closes unexpectedly, COGBox says what the exit code means. It also offers to make a
problem report: one zip on your desktop with the log, the settings and a description of your PC.
It has no saves and no game files, and your Windows user name is taken out. Nothing is sent
anywhere. Attach it to an issue at https://github.com/Shagg88/COGBox/issues


Licences
--------
COGBox and the ports' own code are under the MIT licence (LICENSE.txt). The libraries, fonts and
vendor upscalers included (SDL3, Dear ImGui, miniz, stb, the rexglue SDK, AMD FidelityFX, NVIDIA
DLSS, Intel XeSS, the DirectX Shader Compiler) come under their own terms, in the licenses folder.
Fonts: Anton, Barlow and Barlow Condensed, under the SIL Open Font License 1.1.
