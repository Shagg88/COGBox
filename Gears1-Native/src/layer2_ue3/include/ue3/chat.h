// Layer 2 (Unreal Engine 3): the in-match chat's engine side (PR #421).
// Sending: the game's own `Say` / `TeamSay` through ue3/console.h's trusted queue (no cheat gate, no notice) - the host
// passes it on to everyone / the team (UE3's GameInfo.Broadcast / BroadcastTeam), each player's PlayerController.
// TeamMessage puts "Name#1234: text" in its HUD's ConsoleMessages (TArray of 24-byte entries: FString Text, FColor,
// float MessageLife, PlayerReplicationInfo* - same in all four games, 2026-11-21 probes).
// Receiving: once a frame on the game thread, only in a match and with --cogbox_match_chat on, the players' lines in the
// local player's HUD's ConsoleMessages are taken and their text made "" in place, so the engine draws nothing for them
// (Gears 1 would in a match) and still frees them itself. Entries without a PlayerReplicationInfo are the engine's own
// text ("Open Main Menu"...) and are left alone, as is the whole list outside a match.
// The entries do not say Say from TeamSay, so a team message carries xenos/chat_logic.h's team marker in front of
// its text (the overlay takes it off and draws the line as a team line). Header kept identical in the four games.
// Our code (MIT).
#pragma once

#include <cstdint>
#include <string>

struct PPCContext;

struct Ue3ChatReceived {
  std::string raw;   // "Name#1234: text"
  size_t name_len = 0;   // the sender's PlayerName length (it may hold ": " itself)
  bool team = false;   // (the overlay reads the team marker in `raw`)
};

struct Ue3ChatConfig {
  const char* game = "";                   // for the log
  uint32_t engine_global = 0;              // GEngine's address
  uint32_t players_data_offset = 0;        // UGameEngine::GamePlayers data pointer / count (as the console target)
  uint32_t players_num_offset = 0;
  bool (*open_objects)() = nullptr;        // ue3::obj::Init with this game's globals; worker thread, retried
  bool quote_text = false;                 // Gears 1: its exec takes one word unless the text is in quotes
};

// Layer 3, OnPostSetup (next to ue3::match::Configure, with the same values).
void Ue3ChatConfigure(const Ue3ChatConfig& config);
// Any thread. Cleans the text (xenos/chat_logic.h CleanText); false when nothing is left to send or chat is not ready.
bool Ue3ChatSend(const std::string& typed, bool team);
// Game thread, once a frame (straight after Ue3ConsoleRunQueued).
void Ue3ChatPump(PPCContext& ctx, uint8_t* base);
// What the last Ue3ChatPump saw: a match is being played (a match HUD, not a menu / lobby one, and the match has begun
// and not ended). Any thread.
bool Ue3ChatMatchRunning();
// Called on the game thread for each received chat line (set by layer 3 to the overlay's XenosChatReceive).
void Ue3ChatSetSink(void (*sink)(const Ue3ChatReceived& line));
