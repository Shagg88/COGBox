# Gears3-Native - started from the rexglue project template (`rexglue init`, 2026-10-21), laid out like Gears2-Native:
# four layers, each its own library; a layer only sees its own and lower layers' headers.
#   layer 0  Xbox 360 GPU core + game-free tools: Gears 2's src/layer0_xenos, shared (GN_LAYER0_HOME below)
#   layer 1  (not yet)          D3D library profile - Gears 3 is XDK 20500 (Gears 2: 7645), to be written
#   layer 2  src/layer2_ue3    Unreal Engine 3 (launch command line)
#   layer 3  src/layer3_gow3   Gears of War 3 (the executable, game patches)
# Third-party pieces (FLIP's stb_image_write, DXC headers/DLLs, Catch2) are used read-only from Gears 2's third_party
# folder (GN_THIRD_PARTY, set by tools/build.ps1), as agreed with the Gears 2 chat.
cmake_minimum_required(VERSION 3.25)
project(gow3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY ON)
set(GN_THIRD_PARTY "${CMAKE_SOURCE_DIR}/../Gears2-Native/third_party" CACHE PATH "Gears 2's third_party folder (read-only)")
# The shared renderer (layer 0): built from GN_LAYER0_HOME/src/layer0_xenos and its tests from GN_LAYER0_HOME/tests.
# Since Gears 2's afef446 ("One shared renderer", 2026-10-22) both games build ONE copy, Gears 2's, read-only from
# here; renderer changes go to the Gears 2 chat as patches (patches/to-gears2). This folder's own old copy of src/layer0_xenos
# was deleted in job 114 (Shagg 21:55). Override with -DGN_LAYER0_HOME=<folder>.
if(NOT DEFINED GN_LAYER0_HOME)
    set(GN_LAYER0_HOME "${CMAKE_SOURCE_DIR}/../Gears2-Native")
endif()
if(NOT DEFINED GN_G2_DIR)
    set(GN_G2_DIR "${GN_LAYER0_HOME}")
endif()
set(GN_LAYER0_DIR "${GN_LAYER0_HOME}/src/layer0_xenos")
set(GN_LAYER0_TESTS "${GN_LAYER0_HOME}/tests")
message(STATUS "Gears3-Native: layer 0 (renderer) from ${GN_LAYER0_DIR}")

# SDK boilerplate copied from the template's generated/rexglue.cmake (generated/ is never committed).
include(cmake/rexglue.cmake)

# Layer 0: Xbox 360 GPU core and game-free developer tooling (the shared copy in Gears2-Native).
file(GLOB GN_LAYER0_SOURCES CONFIGURE_DEPENDS ${GN_LAYER0_DIR}/*.cpp ${GN_LAYER0_DIR}/renderer/*.cpp ${GN_LAYER0_DIR}/input/*.cpp)
# The vendor-SDK wrappers are added below only when their SDK is there (job 052).
list(FILTER GN_LAYER0_SOURCES EXCLUDE REGEX "(dlss|fsr|xess)_upscaler\\.cpp$")
add_library(gn_layer0_xenos STATIC ${GN_LAYER0_SOURCES})
target_include_directories(gn_layer0_xenos PUBLIC ${GN_LAYER0_DIR}/include)
target_include_directories(gn_layer0_xenos PRIVATE ${GN_THIRD_PARTY}/flip/src/cpp/tool)
target_link_libraries(gn_layer0_xenos PUBLIC rex::runtime)
set(GN_DXC_DIR ${GN_THIRD_PARTY}/dxc-1.9.2607)
target_include_directories(gn_layer0_xenos PRIVATE ${GN_DXC_DIR}/inc)
if(WIN32)
    target_link_libraries(gn_layer0_xenos PRIVATE dbghelp d3d12 dxgi dxguid ole32)
endif()
# Vulkan backend (Gears 2's notes/68), as Gears 2's CMakeLists.txt: the SDK's Vulkan headers include renderdoc_app.h
# (rex::runtime doesn't pass that folder on), and our backend needs Vulkan headers - a Khronos release in Gears 2's
# third_party (newest first), else the SDK tree's copy; without either it builds its null stub. The SDK install must
# be a Vulkan-ON one (job 125's snapshot) for --gow2_renderer_api=vulkan to do anything.
if(TARGET rex::renderdoc)
    target_link_libraries(gn_layer0_xenos PRIVATE rex::renderdoc)
endif()
file(GLOB GN_VK_HEADER_DIRS LIST_DIRECTORIES true ${GN_THIRD_PARTY}/vulkan-headers-v1.4.*)
list(SORT GN_VK_HEADER_DIRS COMPARE NATURAL ORDER DESCENDING)
set(GN_VULKAN_HEADERS "")
foreach(gn_d ${GN_VK_HEADER_DIRS} ${GN_THIRD_PARTY}/rexglue-sdk/thirdparty/vulkan-headers)
    if(NOT GN_VULKAN_HEADERS AND EXISTS ${gn_d}/include/vulkan/vulkan_core.h)
        set(GN_VULKAN_HEADERS ${gn_d}/include)
    endif()
endforeach()
if(GN_VULKAN_HEADERS)
    message(STATUS "Gears3-Native: Vulkan headers ${GN_VULKAN_HEADERS}")
    target_include_directories(gn_layer0_xenos PRIVATE ${GN_VULKAN_HEADERS})
    target_compile_definitions(gn_layer0_xenos PRIVATE GN_HAVE_VULKAN_HEADERS=1)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(gn_layer0_xenos PRIVATE -ffp-contract=off)
elseif(MSVC)
    target_compile_options(gn_layer0_xenos PRIVATE /fp:precise)
endif()
rexglue_apply_target_settings(gn_layer0_xenos)

# Layer 2: Unreal Engine 3.
add_library(gn_layer2_ue3 STATIC src/layer2_ue3/launch_command_line.cpp src/layer2_ue3/config_bundle.cpp
            src/layer2_ue3/config_install.cpp src/layer2_ue3/package_patch.cpp src/layer2_ue3/lzx.cpp
            src/layer2_ue3/console.cpp)
target_include_directories(gn_layer2_ue3 PUBLIC src/layer2_ue3/include)
target_link_libraries(gn_layer2_ue3 PUBLIC gn_layer0_xenos)
rexglue_apply_target_settings(gn_layer2_ue3)
# notes/78 chapter recipes: Gears 2's package shader reader (patch 0068, game-free UE3 code that already reads Gears 3's
# package layout) and its LZX decoder, built from GN_LAYER0_HOME like layer 0. Only those files get Gears 2's layer 2
# include folder, so this game's own ue3/ headers are not shadowed.
target_sources(gn_layer2_ue3 PRIVATE ${GN_LAYER0_HOME}/src/layer2_ue3/shader_prebuild.cpp
    ${GN_LAYER0_HOME}/src/layer2_ue3/prebuild_lzx.cpp)
set_source_files_properties(${GN_LAYER0_HOME}/src/layer2_ue3/shader_prebuild.cpp
    ${GN_LAYER0_HOME}/src/layer2_ue3/prebuild_lzx.cpp PROPERTIES
    INCLUDE_DIRECTORIES ${GN_LAYER0_HOME}/src/layer2_ue3/include)

# Layer 3: Gears of War 3 (the executable).
set(GOW3_SOURCES
    src/layer3_gow3/main.cpp
    src/layer3_gow3/game_patches.cpp
    src/layer3_gow3/extras.cpp
    src/layer3_gow3/mouse_look.cpp
)
if(WIN32)
    add_executable(gow3 WIN32 ${GOW3_SOURCES})
else()
    add_executable(gow3 ${GOW3_SOURCES})
endif()
target_include_directories(gow3 PRIVATE src/layer3_gow3)
target_link_libraries(gow3 PRIVATE gn_layer2_ue3)
# 0.2.0 (GitHub #9): DirectX 12 Agility SDK - D3D12Core.dll in <exe>\D3D12\, chosen by agility.cpp's exports, so
# Shader Model 6.6 works on Windows 10's older Direct3D 12 runtime too.
set(GN_AGILITY_DIR ${GN_LAYER0_HOME}/third_party/agility-sdk)
if(WIN32 AND EXISTS ${GN_AGILITY_DIR}/build/native/bin/x64/D3D12Core.dll)
    target_sources(gow3 PRIVATE ${GN_LAYER0_DIR}/agility/agility.cpp)
    add_custom_command(TARGET gow3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:gow3>/D3D12
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GN_AGILITY_DIR}/build/native/bin/x64/D3D12Core.dll $<TARGET_FILE_DIR:gow3>/D3D12
        COMMENT "Copying the Agility SDK's D3D12Core.dll to D3D12\\ next to gow3.exe")
else()
    message(WARNING "gow3: no Agility SDK in third_party/agility-sdk - Windows 10 can't run the D3D12 renderer")
endif()
# Title Update 6 build (notes/24, tools\build.ps1 -Tu6): layer 3 picks the TU6 data addresses (gow3_addresses.h).
if(GN_GAME_VERSION STREQUAL "tu6")
    target_compile_definitions(gow3 PRIVATE GOW3_TU6=1)
endif()
# GPU_PLUGINS xenos: the SDK's Xenos GPU emulation, the reference our renderer is compared with (developer builds).
rexglue_setup_target(gow3 GPU_PLUGINS xenos)
add_custom_command(TARGET gow3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${GN_DXC_DIR}/bin/x64/dxcompiler.dll ${GN_DXC_DIR}/bin/x64/dxil.dll $<TARGET_FILE_DIR:gow3>
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${GN_G2_DIR}/assets/glyphs $<TARGET_FILE_DIR:gow3>/assets/glyphs
    COMMENT "Copying dxcompiler.dll, dxil.dll, and assets/glyphs next to gow3.exe")

# Low input lag (Gears 2 patches 0135 / 0136): NVIDIA Reflex through NVAPI and AMD Anti-Lag 2 (header-only), built in
# when Gears 2's third_party has them (read-only, never committed) - the same block as Gears 2's CMakeLists. Their code
# is layer 0's latency_reflex.cpp (picked up by the glob; needs rex/frame_hooks.h = SDK patch 35).
set(GN_NVAPI_DIR ${GN_THIRD_PARTY}/nvapi)
set(GN_NVAPI_FOUND OFF)
if(WIN32 AND EXISTS ${GN_NVAPI_DIR}/nvapi.h AND EXISTS ${GN_NVAPI_DIR}/amd64/nvapi64.lib)
    set(GN_NVAPI_FOUND ON)
endif()
option(GN_WITH_REFLEX "Build NVIDIA Reflex in (needs Gears 2's third_party/nvapi)" ${GN_NVAPI_FOUND})
set(GN_AL2_DIR ${GN_THIRD_PARTY}/antilag2-sdk)
set(GN_AL2_FOUND OFF)
if(WIN32 AND EXISTS ${GN_AL2_DIR}/ffx_antilag2_dx12.h)
    set(GN_AL2_FOUND ON)
endif()
option(GN_WITH_ANTILAG2 "Build AMD Anti-Lag 2 in (needs Gears 2's third_party/antilag2-sdk)" ${GN_AL2_FOUND})
message(STATUS "Gears3-Native: NVIDIA Reflex ${GN_WITH_REFLEX} (NVAPI found: ${GN_NVAPI_FOUND}), AMD Anti-Lag 2 ${GN_WITH_ANTILAG2} (SDK found: ${GN_AL2_FOUND})")
if(GN_WITH_REFLEX AND GN_NVAPI_FOUND)
    target_compile_definitions(gn_layer0_xenos PRIVATE GN_WITH_REFLEX)
    target_include_directories(gn_layer0_xenos PRIVATE ${GN_NVAPI_DIR})
    target_link_libraries(gn_layer0_xenos PRIVATE ${GN_NVAPI_DIR}/amd64/nvapi64.lib)
endif()
if(GN_WITH_ANTILAG2 AND GN_AL2_FOUND)
    target_compile_definitions(gn_layer0_xenos PRIVATE GN_WITH_ANTILAG2)
    target_include_directories(gn_layer0_xenos PRIVATE ${GN_AL2_DIR})
endif()

# Upscalers (job 052, from Gears 2 notes/64 and notes/65): DLSS, FSR and XeSS are built in when their SDKs are in
# Gears 2's third_party folder (read-only, never committed). GN_WITH_DLSS / GN_WITH_FSR / GN_WITH_XESS switch one off.
set(GN_DLSS_DIR ${GN_THIRD_PARTY}/dlss-sdk)
set(GN_FFX_DIR ${GN_THIRD_PARTY}/fidelityfx-sdk/Kits/FidelityFX)
set(GN_XESS_DIR ${GN_THIRD_PARTY}/xess-sdk)
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
option(GN_WITH_DLSS "Build NVIDIA DLSS in (needs Gears 2's third_party/dlss-sdk)" ${GN_DLSS_FOUND})
option(GN_WITH_FSR "Build AMD FSR in (needs Gears 2's third_party/fidelityfx-sdk)" ${GN_FFX_FOUND})
option(GN_WITH_XESS "Build Intel XeSS in (needs Gears 2's third_party/xess-sdk)" ${GN_XESS_FOUND})
message(STATUS "Gears3-Native: upscalers - DLSS ${GN_WITH_DLSS} (SDK found: ${GN_DLSS_FOUND}), FSR ${GN_WITH_FSR} (SDK found: ${GN_FFX_FOUND}), XeSS ${GN_WITH_XESS} (SDK found: ${GN_XESS_FOUND})")
if(GN_WITH_DLSS AND GN_DLSS_FOUND)
    target_sources(gn_layer0_xenos PRIVATE ${GN_LAYER0_DIR}/renderer/dlss_upscaler.cpp)
    target_compile_definitions(gn_layer0_xenos PRIVATE GN_HAVE_DLSS)
    target_include_directories(gn_layer0_xenos PRIVATE ${GN_DLSS_DIR}/include)
    target_link_libraries(gn_layer0_xenos PRIVATE ${GN_DLSS_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib)
    add_custom_command(TARGET gow3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GN_DLSS_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll $<TARGET_FILE_DIR:gow3>
        COMMENT "Copying nvngx_dlss.dll next to gow3.exe")
endif()
if(GN_WITH_FSR AND GN_FFX_FOUND)
    target_sources(gn_layer0_xenos PRIVATE ${GN_LAYER0_DIR}/renderer/fsr_upscaler.cpp)
    target_compile_definitions(gn_layer0_xenos PRIVATE GN_HAVE_FSR)
    target_include_directories(gn_layer0_xenos PRIVATE ${GN_FFX_DIR}/api/include ${GN_FFX_DIR}/upscalers/include
        ${GN_FFX_DIR}/framegeneration/include)  # Gears 2 patch 0146: AMD FSR frame generation
    # ffx_api_loader.h is written for MSBuild (it checks _WINDOWS). frame_generation.cpp comes in through the glob above.
    set_source_files_properties(${GN_LAYER0_DIR}/renderer/fsr_upscaler.cpp ${GN_LAYER0_DIR}/renderer/frame_generation.cpp
        PROPERTIES COMPILE_DEFINITIONS _WINDOWS)
    add_custom_command(TARGET gow3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GN_FFX_DIR}/signedbin/amd_fidelityfx_loader_dx12.dll
            ${GN_FFX_DIR}/signedbin/amd_fidelityfx_upscaler_dx12.dll
            ${GN_FFX_DIR}/signedbin/amd_fidelityfx_framegeneration_dx12.dll $<TARGET_FILE_DIR:gow3>
        COMMENT "Copying the FidelityFX loader, upscaler and frame-generation DLLs next to gow3.exe")
endif()
if(GN_WITH_XESS AND GN_XESS_FOUND)
    target_sources(gn_layer0_xenos PRIVATE ${GN_LAYER0_DIR}/renderer/xess_upscaler.cpp)
    target_compile_definitions(gn_layer0_xenos PRIVATE GN_HAVE_XESS)
    target_include_directories(gn_layer0_xenos PRIVATE ${GN_XESS_DIR}/inc)
    # No import library: xess_upscaler.cpp loads libxess.dll when XeSS is chosen, so the exe starts without it.
    add_custom_command(TARGET gow3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GN_XESS_DIR}/bin/libxess.dll $<TARGET_FILE_DIR:gow3>
        COMMENT "Copying libxess.dll next to gow3.exe")
endif()

# Profile-guided optimisation (job 143, Shagg 14:20 "You need to set up PGO"; method = Gears 2 patches 0159-0161, mails
# 135500 / 141500). GN_PGO=gen builds execution counters into the recompiled game code and ours; layer 0's
# --gow2_pgo_dump_frame=N (shared shadow_renderer.cpp, patch 0159) writes them to LLVM_PROFILE_FILE at swap N, because the
# harness closes the game. GN_PGO=use builds with the merged profile GN_PGO_PROFILE. irgen / iruse = the IR-level kind.
# With no GN_PGO given, a build uses out\pgo\gow3-<base|tu6>.profdata when a training job has put it there, else it is a
# normal build. The profile is made from game runs, so it stays in out\ and is never committed. Read from the environment
# like Gears 2 (not written to the cache), so a build without it set is the default.
set(GN_PGO "" CACHE STRING "Profile-guided optimisation: empty/auto, off, gen, use, irgen or iruse")
set(GN_PGO_PROFILE "" CACHE FILEPATH "Merged .profdata for GN_PGO=use / iruse")
if(DEFINED ENV{GN_PGO})
    set(GN_PGO "$ENV{GN_PGO}")
endif()
if(DEFINED ENV{GN_PGO_PROFILE})
    set(GN_PGO_PROFILE "$ENV{GN_PGO_PROFILE}")
endif()
if(NOT DEFINED ENV{GN_PGO} AND (GN_PGO STREQUAL "" OR GN_PGO STREQUAL "auto"))
    set(gn_pgo_stored "${CMAKE_SOURCE_DIR}/out/pgo/gow3-${GN_GAME_VERSION}.profdata")
    if(EXISTS "${gn_pgo_stored}")
        set(GN_PGO "use")
        set(GN_PGO_PROFILE "${gn_pgo_stored}")
    else()
        message(STATUS "Gears3-Native: no stored profile ${gn_pgo_stored} - normal build (no PGO)")
    endif()
endif()
set(gn_pgo_targets gow3_recomp gow3 gn_layer0_xenos gn_layer2_ue3)
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
    # clang++ links the profile runtime itself.
    target_link_options(gn_layer0_xenos PUBLIC ${gn_pgo_gen_flag})
    target_link_options(gow3 PRIVATE ${gn_pgo_gen_flag})
    message(STATUS "Gears3-Native: PGO training build (execution counters, ${gn_pgo_gen_flag})")
elseif(GN_PGO STREQUAL "use" OR GN_PGO STREQUAL "iruse")
    if(NOT EXISTS "${GN_PGO_PROFILE}")
        message(FATAL_ERROR "GN_PGO=${GN_PGO}: profile '${GN_PGO_PROFILE}' not found")
    endif()
    # The profile's hash on the command line, so sccache never reuses an object built with an older profile.
    file(MD5 "${GN_PGO_PROFILE}" gn_pgo_md5)
    foreach(gn_t ${gn_pgo_targets})
        if(TARGET ${gn_t})
            target_compile_definitions(${gn_t} PRIVATE GN_PGO_PROFILE_MD5=${gn_pgo_md5})
            target_compile_options(${gn_t} PRIVATE "${gn_pgo_use_flag}=${GN_PGO_PROFILE}" -Wno-profile-instr-unprofiled
                -Wno-profile-instr-out-of-date -Wno-profile-instr-missing -Wno-backend-plugin)
        endif()
    endforeach()
    message(STATUS "Gears3-Native: PGO optimised build (${gn_pgo_use_flag}, profile ${GN_PGO_PROFILE}, md5 ${gn_pgo_md5})")
else()
    message(STATUS "Gears3-Native: PGO off")
endif()

# Unit tests (game-free, Gears 2's layer 0 tests).
option(GN_BUILD_TESTS "Build the unit tests" ON)
if(GN_BUILD_TESTS)
    add_subdirectory(${GN_THIRD_PARTY}/catch2 catch2 EXCLUDE_FROM_ALL)
    add_executable(gn_tests
        ${GN_LAYER0_TESTS}/layer0_formats_test.cpp
        ${GN_LAYER0_TESTS}/layer0_renderer_core_test.cpp
        ${GN_LAYER0_TESTS}/layer0_pipeline_state_test.cpp
        ${GN_LAYER0_TESTS}/layer0_transfer_words_test.cpp
        ${GN_LAYER0_TESTS}/layer0_queries_test.cpp
        ${GN_LAYER0_TESTS}/layer0_colour_formats_test.cpp
        ${GN_LAYER0_TESTS}/layer0_pm4_test.cpp
        ${GN_LAYER0_DIR}/pm4.cpp
        ${GN_LAYER0_TESTS}/layer0_display_gamma_test.cpp
        ${GN_LAYER0_DIR}/display_gamma.cpp)
    target_include_directories(gn_tests PRIVATE ${GN_LAYER0_DIR}/include ${GN_LAYER0_TESTS})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(gn_tests PRIVATE -ffp-contract=off)
    elseif(MSVC)
        target_compile_options(gn_tests PRIVATE /fp:precise)
    endif()
    target_link_libraries(gn_tests PRIVATE Catch2::Catch2WithMain)
endif()

# Game-free shader tools (Gears 2 S3.2 / S3.3 + Gears 3 census).
add_executable(gn_census tools/gn_census.cpp ${GN_LAYER0_DIR}/ucode.cpp)
target_include_directories(gn_census PRIVATE ${GN_LAYER0_DIR}/include)
add_executable(gn_hlsl_check tools/gn_hlsl_check.cpp ${GN_LAYER0_DIR}/ucode.cpp ${GN_LAYER0_DIR}/hlsl.cpp)
target_include_directories(gn_hlsl_check PRIVATE ${GN_LAYER0_DIR}/include)
