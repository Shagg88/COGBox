# Gears2-Native - started from the rexglue project template (`rexglue init`, job 007).
# Four layers (decision D16, roadmap section 1b): each is its own library, and a layer can only see its own and lower
# layers' headers, so a wrong-way dependency fails to build.
cmake_minimum_required(VERSION 3.25)
project(gow2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Only ever use the SDK we point at (CMAKE_PREFIX_PATH / rexglue_DIR from tools/build.ps1), never a stale entry in the
# per-user CMake package registry.
set(CMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY ON)

# SDK boilerplate, copied from the template's generated/rexglue.cmake (see the header of that file).
include(cmake/rexglue.cmake)

# Layer 0: Xbox 360 GPU core and game-free developer tooling.
add_library(gn_layer0_xenos STATIC
    src/layer0_xenos/achievements_data.cpp
    src/layer0_xenos/achievements_export.cpp
    src/layer0_xenos/window_icon.cpp
    src/layer0_xenos/achievements_popup_logic.cpp
    src/layer0_xenos/achievements_browser_model.cpp
    src/layer0_xenos/achievements_browser.cpp
    src/layer0_xenos/achievement_popup_assets.cpp
    src/layer0_xenos/achievement_popup.cpp
    src/layer0_xenos/ui_lang.cpp
    src/layer0_xenos/popup_common.cpp
    src/layer0_xenos/popup_x360.cpp
    src/layer0_xenos/popup_xbox.cpp
    src/layer0_xenos/popup_ps5.cpp
    src/layer0_xenos/crash_report.cpp
    src/layer0_xenos/system_report.cpp
    src/layer0_xenos/save_watch.cpp
    src/layer0_xenos/cpu_check.cpp
    src/layer0_xenos/latency_reflex.cpp
    src/layer0_xenos/renderer/frame_generation.cpp
    src/layer0_xenos/renderer/dlss_framegen.cpp
    src/layer0_xenos/draw_stats.cpp
    src/layer0_xenos/renderer/d3d12_backend.cpp
    src/layer0_xenos/renderer/vulkan_backend.cpp
    src/layer0_xenos/renderer/dxc_compiler.cpp
    src/layer0_xenos/renderer/shader_cache.cpp
    src/layer0_xenos/renderer/render_target_cache.cpp
    src/layer0_xenos/renderer/guest_watch.cpp
    src/layer0_xenos/renderer/texture_cache.cpp
    src/layer0_xenos/renderer/texture_pack.cpp
    src/layer0_xenos/renderer/prompt_atlas_catalog.cpp
    src/layer0_xenos/renderer/prompt_overlay_compositor.cpp
    src/layer0_xenos/input/prompt_device_monitor.cpp
    src/layer0_xenos/renderer/shadow_renderer.cpp
    src/layer0_xenos/renderer/upscaler.cpp
    src/layer0_xenos/renderer/ao.cpp
    src/layer0_xenos/hold_guest.cpp
    src/layer0_xenos/draw_log.cpp
    src/layer0_xenos/ucode.cpp
    src/layer0_xenos/hlsl.cpp
    src/layer0_xenos/dxbc_gen.cpp
    src/layer0_xenos/dxbc_spirv.cpp
    src/layer0_xenos/shader_dump.cpp
    src/layer0_xenos/resolve_log.cpp
    src/layer0_xenos/pm4.cpp
    src/layer0_xenos/pm4_shadow.cpp
    src/layer0_xenos/display_gamma.cpp
    src/layer0_xenos/gpu_system.cpp
    src/layer0_xenos/frame_capture.cpp
    src/layer0_xenos/observer_hub.cpp
    src/layer0_xenos/frame_log.cpp
    src/layer0_xenos/image_dump.cpp
    src/layer0_xenos/script_pad.cpp
    src/layer0_xenos/fps_bar.cpp
    src/layer0_xenos/ao_debug.cpp
    src/layer0_xenos/ingame_menu.cpp
    src/layer0_xenos/window_hotkey.cpp   # GitHub #316
    src/layer0_xenos/menu_look.cpp
    src/layer0_xenos/social_ingame.cpp
    src/layer0_xenos/player_tags.cpp  # GitHub #353: the COGBox tags of the players in a match
    src/layer0_xenos/social_page.cpp
    src/layer0_xenos/social_popup.cpp
    src/layer0_xenos/cheat_popup.cpp
    src/layer0_xenos/chat_logic.cpp          # in-match chat (Y / U)
    src/layer0_xenos/chat_panel.cpp
    src/layer0_xenos/social_shared.cpp
    src/layer0_xenos/profiler.cpp
)
target_include_directories(gn_layer0_xenos PUBLIC src/layer0_xenos/include)
include(${CMAKE_SOURCE_DIR}/cmake/spirv_opt.cmake)  # 0.6.1: SPIRV-Tools' optimiser for the Vulkan shader path
gn_add_spirv_opt(gn_layer0_xenos ${CMAKE_SOURCE_DIR}/third_party)
# stb_image_write for PNG frame capture (public domain / MIT), from the pinned FLIP checkout (job 031).
target_include_directories(gn_layer0_xenos PRIVATE third_party/flip/src/cpp/tool)
target_link_libraries(gn_layer0_xenos PUBLIC rex::runtime)
if(TARGET rex::renderdoc)  # notes/68 V3, job 375: the SDK's Vulkan headers include renderdoc_app.h; rex::runtime
    target_link_libraries(gn_layer0_xenos PRIVATE rex::renderdoc)  # doesn't pass its include folder on
endif()
if(WIN32)
    target_link_libraries(gn_layer0_xenos PRIVATE dbghelp)  # minidumps
endif()
# Our renderer (S3.5): D3D12 + DXGI, ole32 for CoTaskMemFree (DXC version string). DXC is loaded at run time from
# dxcompiler.dll next to gow2.exe (no import library); only its header comes from third_party.
set(GN_DEPS_THIRD_PARTY ${CMAKE_SOURCE_DIR}/third_party)
include(${CMAKE_SOURCE_DIR}/cmake/gn_deps.cmake)   # GN_DXC_DIR, GN_AGILITY_DIR (0.4.9)
target_include_directories(gn_layer0_xenos PRIVATE ${GN_DXC_DIR}/inc)
# Notes/68: the Vulkan backend needs only headers (the loader is opened at run time). Newest first: a Khronos
# Vulkan-Headers release in third_party/vulkan-headers-v1.4.* (not committed, like the other vendor trees), else the
# SDK tree's copy. Without either, vulkan_backend.cpp builds its null stub.
file(GLOB GN_VK_HEADER_DIRS LIST_DIRECTORIES true ${CMAKE_SOURCE_DIR}/third_party/vulkan-headers-v1.4.*)
list(SORT GN_VK_HEADER_DIRS COMPARE NATURAL ORDER DESCENDING)
set(GN_VULKAN_HEADERS "")
foreach(gn_d ${GN_VK_HEADER_DIRS} ${CMAKE_SOURCE_DIR}/third_party/rexglue-sdk/thirdparty/vulkan-headers)
    if(NOT GN_VULKAN_HEADERS AND EXISTS ${gn_d}/include/vulkan/vulkan_core.h)
        set(GN_VULKAN_HEADERS ${gn_d}/include)
    endif()
endforeach()
if(GN_VULKAN_HEADERS)
    message(STATUS "Gears-Native: Vulkan headers ${GN_VULKAN_HEADERS}")
    target_include_directories(gn_layer0_xenos PRIVATE ${GN_VULKAN_HEADERS})
    target_compile_definitions(gn_layer0_xenos PRIVATE GN_HAVE_VULKAN_HEADERS=1)
endif()
if(WIN32)
    target_link_libraries(gn_layer0_xenos PRIVATE d3d12 dxgi dxguid ole32)
endif()
# Same float rules as the renderer-core tests (review of upl-n092): no FMA contraction in code that includes
# xenos/gamma.h / resolve maths, so the game and gn_tests round identically.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(gn_layer0_xenos PRIVATE -ffp-contract=off)
elseif(MSVC)
    target_compile_options(gn_layer0_xenos PRIVATE /fp:precise)
endif()
rexglue_apply_target_settings(gn_layer0_xenos)

# Layer 1: D3D library version profile (XDK 7645). Stage 1 code starts here (resource-call watch).
add_library(gn_layer1_xdk7645 STATIC
    src/layer1_xdk7645/d3d_watch.cpp
    src/layer1_xdk7645/resource_check.cpp
)
target_include_directories(gn_layer1_xdk7645 PUBLIC src/layer1_xdk7645/include)
target_link_libraries(gn_layer1_xdk7645 PUBLIC gn_layer0_xenos)
rexglue_apply_target_settings(gn_layer1_xdk7645)

# Layer 2: Unreal Engine 3.
add_library(gn_layer2_ue3 STATIC
    src/layer2_ue3/launch_command_line.cpp
    src/layer2_ue3/config_bundle.cpp
    src/layer2_ue3/config_install.cpp
    src/layer2_ue3/shader_prebuild.cpp
    src/layer2_ue3/prebuild_lzx.cpp
    src/layer2_ue3/main_menu_exit.cpp
    src/layer2_ue3/package_exports.cpp
    src/layer2_ue3/console.cpp  # GitHub #264: engine console commands (--gow2_cheats, --ue3_hotkeys, --ue3_exec); Gears 3 builds it too
    src/layer2_ue3/objects.cpp  # GitHub #245: the engine's objects by name (ue3/objects.h; Gears 2 only - Gears 1 / 3 have their own)
    src/layer2_ue3/name_tags.cpp  # GitHub #353: COGBox tags after the players' names (Gears 1 / 3 / J build it too)
    src/layer2_ue3/chat.cpp  # in-match chat: Say / TeamSay and the HUD's received lines (Gears 1 / 3 / J build it too)
    src/layer2_ue3/mod_extras.cpp  # --ue3_mods extras on modcore (KiltedKernel's, shared with Gears 3): mod.ini rules, bare files, config fragments
    ${CMAKE_CURRENT_SOURCE_DIR}/../Gears3-Native/src/layer2_ue3/modcore/modcore.cpp  # one level up: Gears2-Native and Gears3-Native are siblings (ia10a)
)
# console.cpp's options are saved under Gears2 here (Gears 3 keeps its Gears3 group)
set_source_files_properties(src/layer2_ue3/console.cpp PROPERTIES COMPILE_DEFINITIONS GN_UE3_CONSOLE_GEARS2=1)
target_include_directories(gn_layer2_ue3 PUBLIC src/layer2_ue3/include)
# modcore's headers (modcore/modcore.h, modcore/campaigns.h) live next to Gears 3's layer 2
target_include_directories(gn_layer2_ue3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Gears3-Native/src/layer2_ue3)
target_link_libraries(gn_layer2_ue3 PUBLIC gn_layer1_xdk7645)
rexglue_apply_target_settings(gn_layer2_ue3)

# Layer 3: Gears of War 2 (the executable).
set(GOW2_SOURCES
    src/layer3_gow2/main.cpp
    src/layer3_gow2/untiled.cpp
    src/layer3_gow2/game_patches.cpp
    src/layer3_gow2/mouse_look.cpp
    src/layer3_gow2/main_menu_exit.cpp
    src/layer3_gow2/unlocks.cpp
    src/layer3_gow2/cheats.cpp
    src/layer3_gow2/adaptive_triggers.cpp  # GitHub #245: DualSense trigger effects per weapon (--pad_adaptive_triggers)
    src/layer3_gow2/first_person.cpp  # --gow2_first_person (experimental first-person camera)
)
if(WIN32)
    # GitHub #355: the game's own window / taskbar icon (res/gow2.rc; SDL takes the exe's first icon)
    enable_language(RC)
    add_executable(gow2 WIN32 ${GOW2_SOURCES} res/gow2.rc)
else()
    add_executable(gow2 ${GOW2_SOURCES})
endif()
target_link_libraries(gow2 PRIVATE gn_layer2_ue3)
# 0.2.0 (GitHub #9): DirectX 12 Agility SDK - D3D12Core.dll in <exe>\D3D12\, chosen by agility.cpp's exports, so
# Shader Model 6.6 works on Windows 10's older Direct3D 12 runtime too.
if(WIN32 AND EXISTS ${GN_AGILITY_DIR}/build/native/bin/x64/D3D12Core.dll)
    target_sources(gow2 PRIVATE ${CMAKE_SOURCE_DIR}/src/layer0_xenos/agility/agility.cpp)
    target_compile_definitions(gow2 PRIVATE GN_AGILITY_SDK_VERSION=${GN_AGILITY_SDK_VERSION})
    add_custom_command(TARGET gow2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:gow2>/D3D12
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GN_AGILITY_DIR}/build/native/bin/x64/D3D12Core.dll $<TARGET_FILE_DIR:gow2>/D3D12
        COMMENT "Copying the Agility SDK's D3D12Core.dll to D3D12\\ next to gow2.exe")
else()
    message(WARNING "gow2: no Agility SDK in third_party/agility-sdk - Windows 10 can't run the D3D12 renderer")
endif()
# Title Update 6 build (notes/63, tools\build.ps1 -Tu6): layer 3 picks the TU6 data addresses (gow2_addresses.h).
if(GN_GAME_VERSION STREQUAL "tu6")
    target_compile_definitions(gow2 PRIVATE GOW2_TU6=1)
endif()

# GPU_PLUGINS xenos: the SDK's Xenos GPU emulation (from Xenia). Kept in developer builds as the reference while our
# own renderer is built (roadmap decision D1); runs with --gpu_plugin=xenos via tools/run.ps1.
rexglue_setup_target(gow2 GPU_PLUGINS xenos)
# CPU work (notes/60). GN_RECOMP_NO_PERF_COUNTERS: the SDK's RelWithDebInfo build defines REXGLUE_ENABLE_PROFILING, so
# every indirect call in the recompiled code bumps one shared atomic counter nothing of ours reads. GN_CPU_ARCH: the CPU
# level our code and the recompiled code are built for (x86-64-v3 by default since 0.6.4); multiply-adds are
# never fused (-ffp-contract=off), as the PowerPC code keeps them separate.
option(GN_RECOMP_NO_PERF_COUNTERS "Recompiled code: no SDK perf counter on every indirect call" ON)
# 0.6.4 (Shagg: "I want to move to the AVX2 only"): x86-64-v3 (AVX2 / FMA / BMI2) is the default for all four games;
# an empty value (an older build folder's cache) means the default too. x86-64 = the old baseline build.
set(GN_CPU_ARCH "x86-64-v3" CACHE STRING "CPU level for our code and the recompiled code: x86-64-v3 (default, AVX2) or x86-64 (baseline)")
if(DEFINED ENV{GN_CPU_ARCH})  # a queue job's choice
    set(GN_CPU_ARCH "$ENV{GN_CPU_ARCH}")
endif()
if(GN_CPU_ARCH STREQUAL "")
    set(GN_CPU_ARCH "x86-64-v3")
endif()
message(STATUS "Gears2-Native: CPU level '${GN_CPU_ARCH}', perf counters off: ${GN_RECOMP_NO_PERF_COUNTERS}")
if(TARGET gow2_recomp AND GN_RECOMP_NO_PERF_COUNTERS)
    target_compile_options(gow2_recomp PRIVATE -UREXGLUE_ENABLE_PROFILING)
endif()
if(GN_CPU_ARCH)
    foreach(gn_t gow2_recomp gow2 gn_layer0_xenos gn_layer1_xdk7645 gn_layer2_ue3)
        if(TARGET ${gn_t})
            target_compile_options(${gn_t} PRIVATE -march=${GN_CPU_ARCH} -ffp-contract=off)
        endif()
    endforeach()
    # Patch 0134: the too-old-CPU check is built for the plain baseline (so it runs on any x64 CPU) and told the level.
    set_source_files_properties(src/layer0_xenos/cpu_check.cpp PROPERTIES
        COMPILE_OPTIONS "-march=x86-64" COMPILE_DEFINITIONS "GN_CPU_LEVEL=\"${GN_CPU_ARCH}\"")
endif()
# Patch 0159 (Shagg 12:04: "start with the ones that will make the biggest difference"): profile-guided optimisation of
# the recompiled game code and ours. GN_PGO=gen builds execution counters in (the game writes them at
# --gow2_pgo_dump_frame to LLVM_PROFILE_FILE); GN_PGO=use builds with the merged profile GN_PGO_PROFILE (.profdata).
# Empty or off = as before. A queue job's environment sets both, like GN_CPU_ARCH (not written to the cache, so a build
# without them is a normal build); the profile is made from runs of the game, so it stays in out\ and is never committed.
set(GN_PGO "" CACHE STRING "Profile-guided optimisation: empty/auto/off, gen, use, irgen or iruse")
set(GN_PGO_PROFILE "" CACHE FILEPATH "Merged .profdata for GN_PGO=use")
if(DEFINED ENV{GN_PGO})
    set(GN_PGO "$ENV{GN_PGO}")
endif()
if(DEFINED ENV{GN_PGO_PROFILE})
    set(GN_PGO_PROFILE "$ENV{GN_PGO_PROFILE}")
endif()
# Patch 0160 made the stored profile the default; 0.6.4 turns that off again (below).
# 0.6.4 (Shagg 16 Nov: "We can just do PGO off" - the stored profiles are from 25 Oct, before AVX2 and patch 110, and job
# 1936's PGO-off Gears 3 ran faster than job 1925's with the profile): no GN_PGO given = a normal build. The next profiles
# are to be trained after the move to the latest rexglue. GN_PGO=use with no GN_PGO_PROFILE takes the stored one (new in 0.6.4).
if(GN_PGO STREQUAL "" OR GN_PGO STREQUAL "auto")
    set(GN_PGO "off")
    message(STATUS "Gears2-Native: PGO off (the default; GN_PGO=use for a profile-guided build)")
endif()
if((GN_PGO STREQUAL "use" OR GN_PGO STREQUAL "iruse") AND GN_PGO_PROFILE STREQUAL "")
    set(gn_pgo_stored "${CMAKE_SOURCE_DIR}/out/pgo/gow2-${GN_GAME_VERSION}.profdata")
    if(EXISTS "${gn_pgo_stored}")
        set(GN_PGO_PROFILE "${gn_pgo_stored}")
    endif()
endif()
set(gn_pgo_targets gow2_recomp gow2 gn_layer0_xenos gn_layer1_xdk7645 gn_layer2_ue3)
# Patch 0161: irgen / iruse = the same with LLVM's IR-level instrumentation (-fprofile-generate / -fprofile-use), which
# counts after inlining and is often a little faster than the front-end kind (gen / use).
set(gn_pgo_gen_flag -fprofile-instr-generate)
set(gn_pgo_use_flag -fprofile-instr-use)
if(GN_PGO STREQUAL "irgen" OR GN_PGO STREQUAL "iruse")
    set(gn_pgo_gen_flag -fprofile-generate)
    set(gn_pgo_use_flag -fprofile-use)
endif()
if(GN_PGO STREQUAL "gen" OR GN_PGO STREQUAL "irgen")
    foreach(gn_t ${gn_pgo_targets})
        if(TARGET ${gn_t})
            target_compile_options(${gn_t} PRIVATE ${gn_pgo_gen_flag})
        endif()
    endforeach()
    target_compile_definitions(gn_layer0_xenos PRIVATE GN_PGO_GEN=1)
    # clang++ links the profile runtime (and -include:__llvm_profile_runtime) itself; PUBLIC so every exe that links the
    # instrumented layer 0 (gow2, the vk / pipeline test tools) gets it.
    target_link_options(gn_layer0_xenos PUBLIC ${gn_pgo_gen_flag})
    target_link_options(gow2 PRIVATE ${gn_pgo_gen_flag})
    message(STATUS "Gears2-Native: PGO training build (execution counters, ${gn_pgo_gen_flag})")
elseif(GN_PGO STREQUAL "use" OR GN_PGO STREQUAL "iruse")
    if(NOT EXISTS "${GN_PGO_PROFILE}")
        message(FATAL_ERROR "GN_PGO=${GN_PGO}: profile '${GN_PGO_PROFILE}' not found (with no GN_PGO_PROFILE, the stored out/pgo profile is used when it exists)")
    endif()
    # Patch 0160: the profile's hash on the command line, so the compiler cache (sccache) never reuses an object built
    # with an older profile at the same path.
    file(MD5 "${GN_PGO_PROFILE}" gn_pgo_md5)
    foreach(gn_t ${gn_pgo_targets})
        if(TARGET ${gn_t})
            target_compile_definitions(${gn_t} PRIVATE GN_PGO_PROFILE_MD5=${gn_pgo_md5})
            target_compile_options(${gn_t} PRIVATE "${gn_pgo_use_flag}=${GN_PGO_PROFILE}" -Wno-profile-instr-unprofiled
                -Wno-profile-instr-out-of-date -Wno-profile-instr-missing -Wno-backend-plugin)
        endif()
    endforeach()
    message(STATUS "Gears2-Native: PGO optimised build (${gn_pgo_use_flag}, profile ${GN_PGO_PROFILE}, md5 ${gn_pgo_md5})")
else()
    message(STATUS "Gears2-Native: PGO off")
endif()
# DXC for our renderer's shader cache: copied next to gow2.exe (dxcompiler.dll loads dxil.dll from there to sign DXIL).
add_custom_command(TARGET gow2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:gow2>/licenses
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/cogbox/licenses/PsychoV35-LICENSE.txt $<TARGET_FILE_DIR:gow2>/licenses/PsychoV35-LICENSE.txt
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/cogbox/licenses/RenoDX-Grading-LICENSE.txt $<TARGET_FILE_DIR:gow2>/licenses/RenoDX-Grading-LICENSE.txt
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${GN_DXC_DIR}/bin/x64/dxcompiler.dll ${GN_DXC_DIR}/bin/x64/dxil.dll $<TARGET_FILE_DIR:gow2>
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets/glyphs $<TARGET_FILE_DIR:gow2>/assets/glyphs
    COMMENT "Copying dxcompiler.dll, dxil.dll, and assets/glyphs next to gow2.exe")

# Upscalers (notes/64, notes/65): DLSS and FSR are built in when their SDKs are in third_party (never committed;
# jobs 250 and 270 download them). GN_WITH_DLSS / GN_WITH_FSR switch one off (e.g. if a future SDK does
# not link with this toolchain).
set(GN_DLSS_DIR ${CMAKE_SOURCE_DIR}/third_party/dlss-sdk)
set(GN_FFX_DIR ${CMAKE_SOURCE_DIR}/third_party/fidelityfx-sdk/Kits/FidelityFX CACHE PATH "AMD FSR SDK Kits/FidelityFX folder (fsr049: -DGN_FFX_DIR=... builds against another copy)")
set(GN_DLSS_FOUND OFF)
if(WIN32 AND EXISTS ${GN_DLSS_DIR}/include/nvsdk_ngx.h AND EXISTS ${GN_DLSS_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib)
    set(GN_DLSS_FOUND ON)
endif()
set(GN_FFX_FOUND OFF)
if(WIN32 AND EXISTS ${GN_FFX_DIR}/api/include/ffx_api.h AND EXISTS ${GN_FFX_DIR}/signedbin/amd_fidelityfx_loader_dx12.dll)
    set(GN_FFX_FOUND ON)
endif()
option(GN_WITH_DLSS "Build NVIDIA DLSS in (needs third_party/dlss-sdk)" ${GN_DLSS_FOUND})
option(GN_WITH_FSR "Build AMD FSR in (needs third_party/fidelityfx-sdk)" ${GN_FFX_FOUND})
message(STATUS "Gears2-Native: upscalers - DLSS ${GN_WITH_DLSS} (SDK found: ${GN_DLSS_FOUND}), FSR ${GN_WITH_FSR} (SDK found: ${GN_FFX_FOUND})")
# Patch 0135: NVIDIA Reflex through NVAPI (github.com/NVIDIA/nvapi), in third_party/nvapi when a job has downloaded it.
set(GN_NVAPI_DIR ${CMAKE_SOURCE_DIR}/third_party/nvapi)
set(GN_NVAPI_FOUND OFF)
if(WIN32 AND EXISTS ${GN_NVAPI_DIR}/nvapi.h AND EXISTS ${GN_NVAPI_DIR}/amd64/nvapi64.lib)
    set(GN_NVAPI_FOUND ON)
endif()
option(GN_WITH_REFLEX "Build NVIDIA Reflex in (needs third_party/nvapi)" ${GN_NVAPI_FOUND})
message(STATUS "Gears2-Native: NVIDIA Reflex ${GN_WITH_REFLEX} (NVAPI found: ${GN_NVAPI_FOUND})")
if(GN_WITH_REFLEX AND GN_NVAPI_FOUND)
    target_compile_definitions(gn_layer0_xenos PRIVATE GN_WITH_REFLEX)
    target_include_directories(gn_layer0_xenos PRIVATE ${GN_NVAPI_DIR})
    target_link_libraries(gn_layer0_xenos PRIVATE ${GN_NVAPI_DIR}/amd64/nvapi64.lib)
endif()
# NVIDIA DLSS frame generation through Streamline (renderer/dlss_framegen.cpp, --gow2_frame_generation=dlss,
# experimental): the few public Streamline 2.14.16 headers it needs are vendored in src/layer0_xenos/streamline (MIT);
# there is no library to link - sl.interposer.dll is loaded at run time when the player has put NVIDIA's DLLs next to
# the exe (never copied by the build). OFF builds keep the same file with empty functions.
option(GN_WITH_STREAMLINE "Build NVIDIA DLSS frame generation (Streamline, loaded at run time) in" ${WIN32})
message(STATUS "Gears2-Native: NVIDIA DLSS frame generation (Streamline) ${GN_WITH_STREAMLINE}")
if(GN_WITH_STREAMLINE AND WIN32)
    target_compile_definitions(gn_layer0_xenos PRIVATE GN_HAVE_STREAMLINE)
    target_include_directories(gn_layer0_xenos PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/layer0_xenos/streamline/include)
endif()
# Patch 0136: AMD Anti-Lag 2 (github.com/GPUOpen-LibrariesAndSDKs/AntiLag2-SDK, header-only) in third_party/antilag2-sdk.
set(GN_AL2_DIR ${CMAKE_SOURCE_DIR}/third_party/antilag2-sdk)
set(GN_AL2_FOUND OFF)
if(WIN32 AND EXISTS ${GN_AL2_DIR}/ffx_antilag2_dx12.h)
    set(GN_AL2_FOUND ON)
endif()
option(GN_WITH_ANTILAG2 "Build AMD Anti-Lag 2 in (needs third_party/antilag2-sdk)" ${GN_AL2_FOUND})
message(STATUS "Gears2-Native: AMD Anti-Lag 2 ${GN_WITH_ANTILAG2} (SDK found: ${GN_AL2_FOUND})")
if(GN_WITH_ANTILAG2 AND GN_AL2_FOUND)
    target_compile_definitions(gn_layer0_xenos PRIVATE GN_WITH_ANTILAG2)
    target_include_directories(gn_layer0_xenos PRIVATE ${GN_AL2_DIR})
endif()
if(GN_WITH_DLSS AND GN_DLSS_FOUND)
    target_sources(gn_layer0_xenos PRIVATE src/layer0_xenos/renderer/dlss_upscaler.cpp)
    target_compile_definitions(gn_layer0_xenos PRIVATE GN_HAVE_DLSS)
    target_include_directories(gn_layer0_xenos PRIVATE ${GN_DLSS_DIR}/include)
    # The dynamic-CRT NGX library (/MD, as the rest of the build).
    target_link_libraries(gn_layer0_xenos PRIVATE ${GN_DLSS_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib)
    add_custom_command(TARGET gow2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GN_DLSS_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll $<TARGET_FILE_DIR:gow2>
        COMMENT "Copying nvngx_dlss.dll next to gow2.exe")
endif()
if(GN_WITH_FSR AND GN_FFX_FOUND)
    target_sources(gn_layer0_xenos PRIVATE src/layer0_xenos/renderer/fsr_upscaler.cpp
        src/layer0_xenos/renderer/fsr_vulkan.cpp)  # fsr049: the Vulkan upscaler (own file: SDK header clash)
    target_compile_definitions(gn_layer0_xenos PRIVATE GN_HAVE_FSR)
    target_include_directories(gn_layer0_xenos PRIVATE ${GN_FFX_DIR}/api/include ${GN_FFX_DIR}/upscalers/include
        ${GN_FFX_DIR}/framegeneration/include)  # patch 0146: frame generation
    # ffx_api_loader.h is written for MSBuild (it checks _WINDOWS).
    target_sources(gn_layer0_xenos PRIVATE src/layer0_xenos/renderer/frame_generation_vk.cpp)  # notes/83: FSR frame generation on Vulkan
    set_source_files_properties(src/layer0_xenos/renderer/fsr_upscaler.cpp src/layer0_xenos/renderer/frame_generation.cpp
        src/layer0_xenos/renderer/frame_generation_vk.cpp
        PROPERTIES COMPILE_DEFINITIONS _WINDOWS)
    # Notes/83: AMD FSR frame generation on Vulkan - AMD's Vulkan loader and frame-generation DLLs, when the SDK has them.
    if(EXISTS ${GN_FFX_DIR}/signedbin/amd_fidelityfx_loader_vk.dll AND EXISTS ${GN_FFX_DIR}/signedbin/amd_fidelityfx_framegeneration_vk.dll)
        add_custom_command(TARGET gow2 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GN_FFX_DIR}/signedbin/amd_fidelityfx_loader_vk.dll
                ${GN_FFX_DIR}/signedbin/amd_fidelityfx_framegeneration_vk.dll $<TARGET_FILE_DIR:gow2>
            COMMENT "Copying the FidelityFX Vulkan loader and frame-generation DLLs next to gow2.exe")
    endif()
    # fsr049: AMD FSR SDK 2.4's Vulkan upscaler (FSR 4.1 / 3.1 on the Vulkan renderer).
    if(EXISTS ${GN_FFX_DIR}/signedbin/amd_fidelityfx_loader_vk.dll AND EXISTS ${GN_FFX_DIR}/signedbin/amd_fidelityfx_upscaler_vk.dll)
        add_custom_command(TARGET gow2 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GN_FFX_DIR}/signedbin/amd_fidelityfx_loader_vk.dll
                ${GN_FFX_DIR}/signedbin/amd_fidelityfx_upscaler_vk.dll $<TARGET_FILE_DIR:gow2>
            COMMENT "Copying the FidelityFX Vulkan loader and upscaler DLLs next to gow2.exe")
    endif()
    add_custom_command(TARGET gow2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GN_FFX_DIR}/signedbin/amd_fidelityfx_loader_dx12.dll
            ${GN_FFX_DIR}/signedbin/amd_fidelityfx_upscaler_dx12.dll
            ${GN_FFX_DIR}/signedbin/amd_fidelityfx_framegeneration_dx12.dll $<TARGET_FILE_DIR:gow2>
        COMMENT "Copying the FidelityFX loader, upscaler and frame-generation DLLs next to gow2.exe")
endif()

# Unit tests (Catch2 from third_party, decision D17). Game-free; build\<config>\gn_tests.exe runs them.
option(GN_BUILD_TESTS "Build the unit tests" ON)
if(GN_BUILD_TESTS)
    add_subdirectory(third_party/catch2 EXCLUDE_FROM_ALL)
    add_executable(gn_tests
        tests/layer0_formats_test.cpp
        tests/layer0_renderer_core_test.cpp
        tests/layer0_pipeline_state_test.cpp
        tests/layer0_transfer_words_test.cpp
        tests/layer0_queries_test.cpp
        tests/layer0_player_tags_test.cpp
        tests/layer0_colour_formats_test.cpp
        tests/layer0_pm4_test.cpp
        src/layer0_xenos/pm4.cpp
        tests/layer0_display_gamma_test.cpp
        src/layer0_xenos/display_gamma.cpp
        tests/layer0_hlsl_compact_test.cpp
        tests/layer0_upscaler_test.cpp
        tests/layer0_texture_cache_test.cpp
        tests/layer0_texture_pack_test.cpp
        src/layer0_xenos/renderer/texture_cache.cpp
        src/layer0_xenos/renderer/texture_pack.cpp
        tests/layer0_render_target_cache_test.cpp
        src/layer0_xenos/renderer/render_target_cache.cpp
        src/layer0_xenos/renderer/guest_watch.cpp
        tests/layer0_glyph_assets_test.cpp
        tests/layer0_prompt_catalog_test.cpp
        src/layer0_xenos/renderer/prompt_atlas_catalog.cpp
        tests/layer0_prompt_compositor_test.cpp
        src/layer0_xenos/renderer/prompt_overlay_compositor.cpp
        tests/layer0_device_monitor_test.cpp
        tests/layer0_window_hotkey_test.cpp   # GitHub #316 (header only)
        src/layer0_xenos/input/prompt_device_monitor.cpp
        tests/layer0_ao_test.cpp
        tests/layer2_config_bundle_test.cpp
        src/layer2_ue3/config_bundle.cpp
        tests/layer2_prebuild_lzx_test.cpp
        src/layer2_ue3/prebuild_lzx.cpp
        tests/layer0_achievements_popup_test.cpp
        src/layer0_xenos/achievements_popup_logic.cpp
        tests/layer0_achievements_browser_test.cpp
        src/layer0_xenos/achievements_browser_model.cpp
        tests/layer0_achievements_data_test.cpp
        src/layer0_xenos/achievements_data.cpp
        tests/layer2_mouse_carry_test.cpp
        tests/layer2_main_menu_exit_test.cpp
        src/layer2_ue3/main_menu_exit.cpp
        tests/layer2_package_exports_test.cpp
        src/layer2_ue3/package_exports.cpp
        src/layer0_xenos/renderer/upscaler.cpp
        src/layer0_xenos/renderer/ao.cpp
        src/layer0_xenos/hlsl.cpp
        src/layer0_xenos/ucode.cpp)
    target_include_directories(gn_tests PRIVATE src/layer0_xenos/include src/layer2_ue3/include tests third_party/flip/src/cpp/tool)
    # Keep float results bit-identical to the references: no fused multiply-add contraction across statements.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(gn_tests PRIVATE -ffp-contract=off)
    elseif(MSVC)
        target_compile_options(gn_tests PRIVATE /fp:precise)
    endif()
    target_link_libraries(gn_tests PRIVATE Catch2::Catch2WithMain rex::runtime)
    rexglue_apply_target_settings(gn_tests)
    # Files the tests write go here (never under AppData / %TEMP%).
    target_compile_definitions(gn_tests PRIVATE GN_TEST_SCRATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}/gn-test-scratch")
    if(WIN32)
        add_custom_command(TARGET gn_tests POST_BUILD
            COMMAND "$<$<BOOL:$<TARGET_RUNTIME_DLLS:gn_tests>>:${CMAKE_COMMAND};-E;copy_if_different;$<TARGET_RUNTIME_DLLS:gn_tests>;$<TARGET_FILE_DIR:gn_tests>>"
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets/glyphs $<TARGET_FILE_DIR:gn_tests>/assets/glyphs
            COMMAND_EXPAND_LISTS
            VERBATIM)
    endif()
endif()

# Stage 3 S3.2: our microcode decoder checked against the SDK's disassembly of a shader corpus (game-free tool;
# run: build\<config>\gn_disasm_check.exe out\harness\shaders). Standard library only, so it compiles ucode.cpp itself.
add_executable(gn_disasm_check tools/gn_disasm_check.cpp src/layer0_xenos/ucode.cpp)
target_include_directories(gn_disasm_check PRIVATE src/layer0_xenos/include)

# Stage 3 S3.3: Xenos -> HLSL translator compile check, DXIL and SPIR-V (game-free tool; needs the release dxc.exe in
# third_party\dxc-<version>; run: gn_hlsl_check <corpus> <out> --dxc <dxc.exe> [--shard i/n]).
add_executable(gn_hlsl_check tools/gn_hlsl_check.cpp src/layer0_xenos/ucode.cpp src/layer0_xenos/hlsl.cpp)
target_include_directories(gn_hlsl_check PRIVATE src/layer0_xenos/include)

# Stage 3 S3.4: GPU (D3D12/DXIL) vs the SDK's CPU ShaderInterpreter (patched source, SDK patches 7-8), per shader.
set(REXGLUE_SDK_SRC ${CMAKE_SOURCE_DIR}/third_party/rexglue-sdk)
add_executable(gn_shader_oracle
    tools/gn_shader_oracle.cpp
    tools/gn_shader_oracle_d3d12.cpp
    tools/gn_shader_oracle_probe.cpp
    src/layer0_xenos/ucode.cpp
    src/layer0_xenos/hlsl.cpp
    ${REXGLUE_SDK_SRC}/src/graphics/pipeline/shader/interpreter.cpp
    ${REXGLUE_SDK_SRC}/src/graphics/register_file.cpp
    ${REXGLUE_SDK_SRC}/src/graphics/format/ucode.cpp
)
target_include_directories(gn_shader_oracle BEFORE PRIVATE tools src/layer0_xenos/include ${REXGLUE_SDK_SRC}/include)
target_include_directories(gn_shader_oracle PRIVATE $<TARGET_PROPERTY:rex::runtime,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(gn_shader_oracle PRIVATE NDEBUG NOMINMAX WIN32_LEAN_AND_MEAN $<TARGET_PROPERTY:rex::runtime,INTERFACE_COMPILE_DEFINITIONS>)
target_compile_features(gn_shader_oracle PRIVATE cxx_std_23)
target_link_libraries(gn_shader_oracle PRIVATE d3d12 dxgi)

# Patch 0059: gn_pipebench (tools/pipebench) - times the graphics driver's pipeline builds from our DXBC, per
# translator variant, on this PC's GPU (no game runs). Not part of the normal build: cmake --build <dir> --target gn_pipebench.
# Notes/68: gn_vktest (tools/vktest) - checks of the Vulkan backend on this PC's GPU, no game. Not in the normal build.
add_executable(gn_vktest EXCLUDE_FROM_ALL tools/vktest/vktest.cpp)
add_executable(gn_hdrtest EXCLUDE_FROM_ALL tools/hdrtest/hdrtest.cpp)
set_target_properties(gn_hdrtest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/hdr-tests")
target_link_libraries(gn_hdrtest PRIVATE gn_layer0_xenos)
rexglue_apply_target_settings(gn_hdrtest)
target_link_libraries(gn_vktest PRIVATE gn_layer0_xenos)
rexglue_apply_target_settings(gn_vktest)
if(GN_VULKAN_HEADERS)  # notes/68 V3: its external-device check makes a device of its own
    target_include_directories(gn_vktest PRIVATE ${GN_VULKAN_HEADERS})
    target_compile_definitions(gn_vktest PRIVATE GN_HAVE_VULKAN_HEADERS=1)
endif()
if(WIN32)
    add_executable(gn_pipebench EXCLUDE_FROM_ALL tools/pipebench/pipebench.cpp)
    target_link_libraries(gn_pipebench PRIVATE gn_layer0_xenos)
    rexglue_apply_target_settings(gn_pipebench)
endif()

# 0.6.6: Intel's upscaler DLLs are no longer built in or shipped. A build folder made before that still holds the
# copies an older build put there; the packager refuses a folder that has them, so every build removes them.
add_custom_command(TARGET gow2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -f $<TARGET_FILE_DIR:gow2>/libxess.dll $<TARGET_FILE_DIR:gow2>/libxess_fg.dll $<TARGET_FILE_DIR:gow2>/libxell.dll
    COMMENT "Removing leftover Intel upscaler DLLs from the build folder")
