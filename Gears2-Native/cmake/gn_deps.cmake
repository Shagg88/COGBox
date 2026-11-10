# 0.4.9 (deps049): which version of each prebuilt vendor SDK the four games build against and copy beside their exe.
# One place for all four projects (Gears2-Native and, through GN_THIRD_PARTY, Gears1/3/J). New versions go in new,
# versioned folders under third_party (job 1415); the old folders stay, and a build falls back to them, with a warning,
# on a PC that does not have the new one yet.
#
# In:  GN_DEPS_THIRD_PARTY = the third_party folder.
# Out: GN_DXC_DIR, GN_AGILITY_DIR + GN_AGILITY_SDK_VERSION (agility.cpp's D3D12SDKVersion export; it MUST match the
#      D3D12Core.dll copied to <exe>\D3D12\, or Direct3D 12 ignores the bundled runtime), GN_XESS_DIR.
if(NOT GN_DEPS_THIRD_PARTY)
    message(FATAL_ERROR "gn_deps.cmake: set GN_DEPS_THIRD_PARTY first")
endif()
set(_tp ${GN_DEPS_THIRD_PARTY})

# DirectX Shader Compiler (official release zip: bin\x64\dxcompiler.dll + dxil.dll, inc\dxcapi.h).
set(GN_DXC_DIR ${_tp}/dxc-1.9.2609)
if(NOT EXISTS ${GN_DXC_DIR}/bin/x64/dxcompiler.dll)
    message(WARNING "no ${GN_DXC_DIR} (job 1415) - using third_party/dxc-1.9.2607")
    set(GN_DXC_DIR ${_tp}/dxc-1.9.2607)
endif()

# DirectX 12 Agility SDK (NuGet Microsoft.Direct3D.D3D12, unpacked: build\native\bin\x64\D3D12Core.dll).
set(GN_AGILITY_DIR ${_tp}/agility-sdk-1.620.1)
set(GN_AGILITY_SDK_VERSION 620)
if(NOT EXISTS ${GN_AGILITY_DIR}/build/native/bin/x64/D3D12Core.dll)
    message(WARNING "no ${GN_AGILITY_DIR} (job 1415) - using third_party/agility-sdk (1.614.0)")
    set(GN_AGILITY_DIR ${_tp}/agility-sdk)
    set(GN_AGILITY_SDK_VERSION 614)
endif()

# Intel XeSS SDK (official release zip). Upscaler only for now (libxess.dll); the same folder has XeFG (libxess_fg.dll,
# inc\xess_fg) and XeLL (libxell.dll, inc\xell) for the later frame-generation work, so it is kept whole.
set(GN_XESS_DIR "")
file(GLOB _xess_cands LIST_DIRECTORIES true ${_tp}/xess-sdk-3.0.2 ${_tp}/xess-sdk-3.0.2/*)
foreach(_d IN LISTS _xess_cands)
    if(NOT GN_XESS_DIR AND EXISTS ${_d}/inc/xess/xess_d3d12.h AND EXISTS ${_d}/bin/libxess.dll)
        set(GN_XESS_DIR ${_d})
    endif()
endforeach()
if(NOT GN_XESS_DIR)
    message(WARNING "no XeSS SDK 3.0.2 in ${_tp}/xess-sdk-3.0.2 (job 1415) - using third_party/xess-sdk")
    set(GN_XESS_DIR ${_tp}/xess-sdk)
endif()
message(STATUS "vendor SDKs: DXC ${GN_DXC_DIR}, Agility ${GN_AGILITY_DIR} (D3D12SDKVersion ${GN_AGILITY_SDK_VERSION}), XeSS ${GN_XESS_DIR}")
