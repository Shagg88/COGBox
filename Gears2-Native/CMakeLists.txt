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
    src/layer0_xenos/crash_report.cpp
    src/layer0_xenos/system_report.cpp
    src/layer0_xenos/cpu_check.cpp
    src/layer0_xenos/latency_reflex.cpp
    src/layer0_xenos/renderer/frame_generation.cpp
    src/layer0_xenos/draw_stats.cpp
    src/layer0_xenos/prim_detail.cpp
    src/layer0_xenos/renderer/d3d12_backend.cpp
    src/layer0_xenos/renderer/vulkan_backend.cpp
    src/layer0_xenos/renderer/dxc_compiler.cpp
    src/layer0_xenos/renderer/shader_cache.cpp
    src/layer0_xenos/renderer/render_target_cache.cpp
    src/layer0_xenos/renderer/guest_watch.cpp
    src/layer0_xenos/renderer/texture_cache.cpp
    src/layer0_xenos/renderer/shadow_renderer.cpp
    src/layer0_xenos/renderer/upscaler.cpp
    src/layer0_xenos/hold_guest.cpp
    src/layer0_xenos/draw_log.cpp
    src/layer0_xenos/shader_watch.cpp
    src/layer0_xenos/ucode.cpp
    src/layer0_xenos/hlsl.cpp
    src/layer0_xenos/dxbc_gen.cpp
    src/layer0_xenos/dxbc_spirv.cpp
    src/layer0_xenos/shader_dump.cpp
    src/layer0_xenos/buffer_dump.cpp
    src/layer0_xenos/resolve_log.cpp
    src/layer0_xenos/query_log.cpp
    src/layer0_xenos/state_coverage.cpp
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
    src/layer0_xenos/ingame_menu.cpp
    src/layer0_xenos/profiler.cpp
)
target_include_directories(gn_layer0_xenos PUBLIC src/layer0_xenos/include)
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
set(GN_DXC_DIR ${CMAKE_SOURCE_DIR}/third_party/dxc-1.9.2607)
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
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/shader_cache_identity.cmake")
cogbox_shader_cache_identity(gn_layer0_xenos "${CMAKE_CURRENT_SOURCE_DIR}/src/layer0_xenos")

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
    src/layer2_ue3/lzx.cpp
)
target_include_directories(gn_layer2_ue3 PUBLIC src/layer2_ue3/include)
target_link_libraries(gn_layer2_ue3 PUBLIC gn_layer1_xdk7645)
rexglue_apply_target_settings(gn_layer2_ue3)

# Layer 3: Gears of War 2 (the executable).
set(GOW2_SOURCES
    src/layer3_gow2/main.cpp
    src/layer3_gow2/untiled.cpp
    src/layer3_gow2/game_patches.cpp
    src/layer3_gow2/mouse_look.cpp
)
if(WIN32)
    add_executable(gow2 WIN32 ${GOW2_SOURCES})
else()
    add_executable(gow2 ${GOW2_SOURCES})
endif()
target_link_libraries(gow2 PRIVATE gn_layer2_ue3)
# 0.2.0 (GitHub #9): DirectX 12 Agility SDK - D3D12Core.dll in <exe>\D3D12\, chosen by agility.cpp's exports, so
# Shader Model 6.6 works on Windows 10's older Direct3D 12 runtime too.
set(GN_AGILITY_DIR ${CMAKE_SOURCE_DIR}/third_party/agility-sdk)
if(WIN32 AND EXISTS ${GN_AGILITY_DIR}/build/native/bin/x64/D3D12Core.dll)
    target_sources(gow2 PRIVATE ${CMAKE_SOURCE_DIR}/src/layer0_xenos/agility/agility.cpp)
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
# level our code and the recompiled code are built for (empty = the compiler's x86-64 baseline); multiply-adds are
# never fused (-ffp-contract=off), as the PowerPC code keeps them separate.
option(GN_RECOMP_NO_PERF_COUNTERS "Recompiled code: no SDK perf counter on every indirect call" ON)
set(GN_CPU_ARCH "" CACHE STRING "CPU level for our code and the recompiled code, e.g. x86-64-v3 (empty = baseline)")
if(DEFINED ENV{GN_CPU_ARCH})  # a queue job's choice (job 218 checks the CPU for AVX2 first)
    set(GN_CPU_ARCH "$ENV{GN_CPU_ARCH}")
endif()
message(STATUS "Gears2-Native: CPU level '${GN_CPU_ARCH}' (empty = x86-64 baseline), perf counters off: ${GN_RECOMP_NO_PERF_COUNTERS}")
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
set(GN_PGO "" CACHE STRING "Profile-guided optimisation: empty/off, gen or use")
set(GN_PGO_PROFILE "" CACHE FILEPATH "Merged .profdata for GN_PGO=use")
if(DEFINED ENV{GN_PGO})
    set(GN_PGO "$ENV{GN_PGO}")
endif()
if(DEFINED ENV{GN_PGO_PROFILE})
    set(GN_PGO_PROFILE "$ENV{GN_PGO_PROFILE}")
endif()
# Patch 0160 (Shagg 12:58 "PGO should be the default"): with no GN_PGO given, a build uses the game version's stored
# profile out\pgo\gow2-<base|tu6>.profdata when it is there (a training job puts it there) and is a normal build when it
# is not. GN_PGO=off / gen / use given by a job still win.
if(NOT DEFINED ENV{GN_PGO} AND (GN_PGO STREQUAL "" OR GN_PGO STREQUAL "auto"))
    set(gn_pgo_stored "${CMAKE_SOURCE_DIR}/out/pgo/gow2-${GN_GAME_VERSION}.profdata")
    if(EXISTS "${gn_pgo_stored}")
        set(GN_PGO "use")
        set(GN_PGO_PROFILE "${gn_pgo_stored}")
    else()
        message(STATUS "Gears2-Native: no stored profile ${gn_pgo_stored} - normal build (no PGO)")
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
        message(FATAL_ERROR "GN_PGO=${GN_PGO}: profile '${GN_PGO_PROFILE}' not found")
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
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${GN_DXC_DIR}/bin/x64/dxcompiler.dll ${GN_DXC_DIR}/bin/x64/dxil.dll $<TARGET_FILE_DIR:gow2>
    COMMENT "Copying dxcompiler.dll + dxil.dll next to gow2.exe")

# Upscalers (notes/64, notes/65): DLSS, FSR and XeSS are built in when their SDKs are in third_party (never committed;
# jobs 250 and 270 download them). GN_WITH_DLSS / GN_WITH_FSR / GN_WITH_XESS switch one off (e.g. if a future SDK does
# not link with this toolchain).
set(GN_DLSS_DIR ${CMAKE_SOURCE_DIR}/third_party/dlss-sdk)
set(GN_FFX_DIR ${CMAKE_SOURCE_DIR}/third_party/fidelityfx-sdk/Kits/FidelityFX)
set(GN_XESS_DIR ${CMAKE_SOURCE_DIR}/third_party/xess-sdk)
set(GN_DLSS_FOUND OFF)
if(WIN32 AND EXISTS ${GN_DLSS_DIR}/include/nvsdk_ngx.h AND EXISTS ${GN_DLSS_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib)
    set(GN_DLSS_FOUND ON)
endif()
set(GN_FFX_FOUND OFF)
if(WIN32 AND EXISTS ${GN_FFX_DIR}/api/include/ffx_api.h AND EXISTS ${GN_FFX_DIR}/signedbin/amd_fidelityfx_loader_dx12.dll)
    set(GN_FFX_FOUND ON)
endif()
set(GN_XESS_FOUND OFF)
if(WIN32 AND EXISTS ${GN_XESS_DIR}/inc/xess/xess_d3d12.h AND EXISTS ${GN_XESS_DIR}/bin/libxess.dll)
    set(GN_XESS_FOUND ON)
endif()
option(GN_WITH_DLSS "Build NVIDIA DLSS in (needs third_party/dlss-sdk)" ${GN_DLSS_FOUND})
option(GN_WITH_FSR "Build AMD FSR in (needs third_party/fidelityfx-sdk)" ${GN_FFX_FOUND})
option(GN_WITH_XESS "Build Intel XeSS in (needs third_party/xess-sdk)" ${GN_XESS_FOUND})
message(STATUS "Gears2-Native: upscalers - DLSS ${GN_WITH_DLSS} (SDK found: ${GN_DLSS_FOUND}), FSR ${GN_WITH_FSR} (SDK found: ${GN_FFX_FOUND}), XeSS ${GN_WITH_XESS} (SDK found: ${GN_XESS_FOUND})")
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
    target_sources(gn_layer0_xenos PRIVATE src/layer0_xenos/renderer/fsr_upscaler.cpp)
    target_compile_definitions(gn_layer0_xenos PRIVATE GN_HAVE_FSR)
    target_include_directories(gn_layer0_xenos PRIVATE ${GN_FFX_DIR}/api/include ${GN_FFX_DIR}/upscalers/include
        ${GN_FFX_DIR}/framegeneration/include)  # patch 0146: frame generation
    # ffx_api_loader.h is written for MSBuild (it checks _WINDOWS).
    set_source_files_properties(src/layer0_xenos/renderer/fsr_upscaler.cpp src/layer0_xenos/renderer/frame_generation.cpp
        PROPERTIES COMPILE_DEFINITIONS _WINDOWS)
    add_custom_command(TARGET gow2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GN_FFX_DIR}/signedbin/amd_fidelityfx_loader_dx12.dll
            ${GN_FFX_DIR}/signedbin/amd_fidelityfx_upscaler_dx12.dll
            ${GN_FFX_DIR}/signedbin/amd_fidelityfx_framegeneration_dx12.dll $<TARGET_FILE_DIR:gow2>
        COMMENT "Copying the FidelityFX loader, upscaler and frame-generation DLLs next to gow2.exe")
endif()
if(GN_WITH_XESS AND GN_XESS_FOUND)
    target_sources(gn_layer0_xenos PRIVATE src/layer0_xenos/renderer/xess_upscaler.cpp)
    target_compile_definitions(gn_layer0_xenos PRIVATE GN_HAVE_XESS)
    target_include_directories(gn_layer0_xenos PRIVATE ${GN_XESS_DIR}/inc)
    # No import library: xess_upscaler.cpp loads libxess.dll when XeSS is chosen, so the exe starts without it.
    add_custom_command(TARGET gow2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GN_XESS_DIR}/bin/libxess.dll $<TARGET_FILE_DIR:gow2>
        COMMENT "Copying libxess.dll next to gow2.exe")
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
        tests/layer0_colour_formats_test.cpp
        tests/layer0_pm4_test.cpp
        src/layer0_xenos/pm4.cpp
        tests/layer0_display_gamma_test.cpp
        src/layer0_xenos/display_gamma.cpp
        tests/layer0_hlsl_compact_test.cpp
        tests/layer0_upscaler_test.cpp
        tests/layer0_shader_progress_test.cpp
        tests/layer0_shader_io_test.cpp
        tests/layer2_config_bundle_test.cpp
        tests/layer2_shader_prebuild_test.cpp
        src/layer2_ue3/lzx.cpp
        src/layer2_ue3/config_bundle.cpp
        src/layer0_xenos/renderer/upscaler.cpp
        src/layer0_xenos/hlsl.cpp
        src/layer0_xenos/ucode.cpp)
    target_include_directories(gn_tests PRIVATE src/layer0_xenos/include src/layer2_ue3/include tests)
    # Keep float results bit-identical to the references: no fused multiply-add contraction across statements.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(gn_tests PRIVATE -ffp-contract=off)
    elseif(MSVC)
        target_compile_options(gn_tests PRIVATE /fp:precise)
    endif()
    target_link_libraries(gn_tests PRIVATE Catch2::Catch2WithMain)
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
