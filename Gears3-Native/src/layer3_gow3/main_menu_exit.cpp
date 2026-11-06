// SPDX-License-Identifier: BSD-3-Clause
// gow3 - ReXGlue Recompiled Project
//
// Exit Game on the main menu. The Coalesced edit renames the defunct "Xbox LIVE
// Marketplace" entry; selecting it makes the game look up the "NeedLiveForDownloadableContent"
// error text (sub_8230CF58 in TU6, Localize) before showing that error. That lookup ends the process
// instead - only while the front end is loaded (ue3::InFrontEndForExit: GearStart*.xxx is the last level
// package opened), so the same message in a level can't close the game.

#include "main_menu_exit.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <cstring>
#include <filesystem>
#include <string_view>

#include <rex/filesystem/vfs.h>  // REX_FILESYSTEM_FILE_OPEN_OBSERVER
#include <rex/logging.h>
#include <rex/ppc.h>
#include <ue3/main_menu_exit.h>
#include <ue3/objects.h>

#include "gow3_addresses.h"

void Ue3ShaderPrebuildChainFileOpened(void (*listener)(std::string_view, const std::filesystem::path*));  // shader_prebuild.cpp

namespace rex::glue {
namespace {

constexpr char kNeedLiveKey[] = "NeedLiveForDownloadableContent";

// The whole key, terminator included (wide, as Localize gets it).
bool GuestUtf16Is(const uint8_t* base, uint32_t addr, const char* text) {
  if (!addr || !base) return false;
  for (uint32_t i = 0;; ++i) {
    const uint16_t c = uint16_t((base[addr + 2 * i] << 8) | base[addr + 2 * i + 1]);
    if (c != uint8_t(text[i])) return false;
    if (!c) return true;
  }
}

void OnFileOpened(std::string_view guest_path, const std::filesystem::path*) {
  ue3::NoteFileOpenedForExit(guest_path);
}

}  // namespace

bool MatchesNeedLive(const uint8_t* base, uint32_t addr) {
  if (!addr || !base) return false;
  if (GuestUtf16Is(base, addr, kNeedLiveKey)) return true;
  // An 8-bit key: the whole key, terminator included.
  return std::memcmp(base + addr, kNeedLiveKey, sizeof(kNeedLiveKey)) == 0;
}

void ExitGame() {
  REXLOG_INFO("MAINMENU: Exit Game selected, ending the process");
  rex::FlushLogging();
#ifdef _WIN32
  TerminateProcess(GetCurrentProcess(), 0);
#else
  _exit(0);
#endif
}

}  // namespace rex::glue

void Gow3InstallMainMenuExitIfRequested() {
  if (!REXCVAR_GET(gow_main_menu_exit)) return;
#if defined(REX_FILESYSTEM_FILE_OPEN_OBSERVER)
  Ue3ShaderPrebuildChainFileOpened(&rex::glue::OnFileOpened);
  REXLOG_INFO("main_menu_exit: file-open hook installed (Exit Game while the front end is loaded)");
#else
  REXLOG_WARN("main_menu_exit: this SDK has no file-open observer (SDK patch 32); Exit Game does nothing");
#endif
}

#if defined(GOW3_TU6)
#define GOW3_NEED_LIVE_FN sub_8230CF58
#define GOW3_NEED_LIVE_IMP __imp__sub_8230CF58
#else
#define GOW3_NEED_LIVE_FN sub_822FC8D0
#define GOW3_NEED_LIVE_IMP __imp__sub_822FC8D0
#endif

extern "C" void GOW3_NEED_LIVE_IMP(PPCContext& ctx, uint8_t* base);

extern "C" void GOW3_NEED_LIVE_FN(PPCContext& ctx, uint8_t* base) {
  if (REXCVAR_GET(gow_main_menu_exit) && rex::glue::MatchesNeedLive(base, ctx.r5.u32)) {
    // GitHub #215: back on the main menu after playing, Gears 3 does not open GearStart*.xxx again (the front end stays
    // loaded), so the file-open tracking still says "in a level" and the entry showed the Marketplace message. A
    // second test, only when the tracking says level: no live player pawn (GearPawn) = the front end.
    bool front = ue3::InFrontEndForExit();
    if (!front) {
      namespace obj = ue3::obj;
      if (obj::Ready() || obj::Init(gow3::addr::kGNamesVar, gow3::addr::kGObjectsVar)) {
        const uint32_t pawn = obj::FindClass("GearPawn");
        const size_t n = pawn ? obj::FindInstancesOf(pawn).size() : size_t(1);
        REXLOG_INFO("MAINMENU: #215 file tracking says level; GearPawn class {:08X}, live pawns {}", pawn, n);
        front = pawn && n == 0;
      }
    }
    if (front) {
      REXLOG_INFO("MAINMENU: NeedLiveForDownloadableContent on the front end (lr {:08X})",
                  static_cast<uint32_t>(ctx.lr));
      rex::glue::ExitGame();
    }
    REXLOG_INFO("MAINMENU: NeedLiveForDownloadableContent in a level (lr {:08X}) - not Exit Game",
                static_cast<uint32_t>(ctx.lr));
  }
  GOW3_NEED_LIVE_IMP(ctx, base);
}
