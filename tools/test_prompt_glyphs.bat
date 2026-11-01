@echo off
setlocal
REM Automated verification suite for 4x Prompt Glyphs, Multi-Hardware Detection, and Anti-Reversion Fix

set "TEST_EXE=%~dp0..\Gears2-Native\build\gn_tests.exe"
if not exist "%TEST_EXE%" set "TEST_EXE=%~dp0..\Gears2-Native\build\RelWithDebInfo\gn_tests.exe"
if not exist "%TEST_EXE%" set "TEST_EXE=%~dp0..\Gears2-Native\build\Release\gn_tests.exe"

if not exist "%TEST_EXE%" (
    echo [ERROR] Test executable gn_tests.exe not found in Gears2-Native\build
    exit /b 1
)

echo Running tests with: "%TEST_EXE%"

echo.
echo === 1. Verifying Texture Cache Anti-Reversion Fix (Slot Reuse ^& Generation Tracking) ===
"%TEST_EXE%" "[texture_cache]"
if errorlevel 1 exit /b 1

echo.
echo === 2. Verifying Kenney CC0 Glyph Asset Packs (Xbox, PlayStation, Switch, Steam Deck, Steam Controller, Keyboard) ===
"%TEST_EXE%" "[assets]"
if errorlevel 1 exit /b 1

echo.
echo === 3. Verifying Prompt Atlas Catalog (Gears 2, 3, Judgment HUD, Font Sheets, Chainsaw Duels) ===
"%TEST_EXE%" "[catalog]"
if errorlevel 1 exit /b 1

echo.
echo === 4. Verifying 4x Catmull-Rom Prompt Overlay Compositor ^& Zero-Hitch Pre-Allocated Variants ===
"%TEST_EXE%" "[compositor]"
if errorlevel 1 exit /b 1

echo.
echo === 5. Verifying Multi-Hardware Input Monitor ^& CVar Style Overrides ===
"%TEST_EXE%" "[input]"
if errorlevel 1 exit /b 1

echo.
echo === ALL PROMPT GLYPH ^& ANTI-REVERSION TESTS PASSED ===
exit /b 0
