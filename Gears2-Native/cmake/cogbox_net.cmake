# cmake/cogbox_net.cmake
# COGBox P2P Matchmaking & NAT Traversal Subsystem Dependencies

if(TARGET miniupnpc OR TARGET libminiupnpc-static)
    return()
endif()

include(FetchContent)

# 1. miniupnp (miniupnpc_2_3_3)
set(UPNPC_BUILD_STATIC ON CACHE BOOL "Build static miniupnpc library" FORCE)
set(UPNPC_BUILD_SHARED OFF CACHE BOOL "Do not build shared miniupnpc library" FORCE)
set(UPNPC_BUILD_TESTS OFF CACHE BOOL "Do not build miniupnpc tests" FORCE)

FetchContent_Declare(
    miniupnp
    GIT_REPOSITORY https://github.com/miniupnp/miniupnp.git
    GIT_TAG miniupnpc_2_3_3
    SOURCE_SUBDIR miniupnpc
)

# 2. libjuice (v1.6.0)
set(NO_SERVER ON CACHE BOOL "Disable libjuice server support" FORCE)
set(NO_TESTS ON CACHE BOOL "Disable libjuice test suite" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static libraries" FORCE)

FetchContent_Declare(
    libjuice
    GIT_REPOSITORY https://github.com/paullouisageneau/libjuice.git
    GIT_TAG v1.6.0
)

# 3. ixwebsocket (v13.7.4 requested; upstream release tag is v12.1.0)
set(USE_TLS OFF CACHE BOOL "Disable external TLS for ixwebsocket" FORCE)
set(USE_ZLIB OFF CACHE BOOL "Disable zlib for ixwebsocket" FORCE)

FetchContent_Declare(
    ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG v12.1.0 # ixwebsocket v13.7.4 (fallback to v12.1.0)
)

# 4. nlohmann_json (v3.11.3)
set(JSON_BuildTests OFF CACHE BOOL "Do not build nlohmann_json tests" FORCE)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)

# 5. googletest (v1.14.0) for C++ unit test runner
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)

FetchContent_MakeAvailable(miniupnp libjuice ixwebsocket nlohmann_json googletest)

# Standardize miniupnpc target name and include paths
if(TARGET libminiupnpc-static)
    target_include_directories(libminiupnpc-static INTERFACE
        $<BUILD_INTERFACE:${miniupnp_SOURCE_DIR}>
        $<BUILD_INTERFACE:${miniupnp_SOURCE_DIR}/miniupnpc>
        $<BUILD_INTERFACE:${miniupnp_SOURCE_DIR}/miniupnpc/include>
    )
    if(NOT TARGET miniupnpc)
        add_library(miniupnpc ALIAS libminiupnpc-static)
    endif()
endif()
