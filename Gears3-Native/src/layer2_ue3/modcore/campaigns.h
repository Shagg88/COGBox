#pragma once
// modcore campaigns (mod API level 1): campaigns mods declare, given act and chapter slots, written as config. MIT.
//
// A mod declares a campaign and its chapters in mod.ini; it never writes slot numbers:
//
//   [Mod]
//   Api = 1
//   [Campaign]
//   Id = gears1                  stable id: chapters of one campaign may come from several mods, joined by it
//   Title = Gears of War         the act's name in Select Chapter
//   Order = 10                   where it sorts among campaigns (the first one its mods give, else 100; then the mods'
//                                load order)
//   Screenshot = <package.texture>
//   [Chapter.1]                  N: the chapter's place in the campaign and its "Chapter N"
//   Map = GOW1_SP_Prison_P
//   Title = 14 Years After
//   Location = Jacinto Maximum Security Prison
//   Time = Emergence Day + 14 years
//   Screenshot = <package.texture>
//   ContentPackages = MPCH_COGDomG2MP_SF, MPCH_COGMarcusG2MP_SF
//
// Slots: campaigns in (Order, first mod's load order, Id) order take the act numbers after the last act the config
// already has (the game's five, RAAM's Shadow's, acts of mods that still write their own), contiguous as Gears 3's
// ConstructActList needs; their chapters, in N order, take the free chapter numbers from 32 (CHAP_DLC_1) to 49. The same
// enabled mods in the same order always get the same slots, so the game's slot-keyed progress and checkpoints stay
// valid (and co-op players agree, their mods being compared already).
#include "modcore.h"

#include <set>

namespace modcore
{
    constexpr int kModApiLevel = 1;   // the mod API this modcore implements (mod.ini Api = ...): 1 = campaigns

    // the acts and chapters a packed configuration (Coalesced_<LANG>.bin) already declares: GearCampaignActData
    // ActNum and GearCampaignChapterData ChapterNum values in its Xbox360-GearGame.ini
    struct UsedSlots { std::set<int> acts, chapters; };
    UsedSlots UsedCampaignSlots(const std::vector<uint8_t>& coalesced);

    // the localized GearGame file a packed configuration carries ("GearGame.int", "GearGame.fra", ...): where the
    // menu's names go. "GearGame.int" when it has none.
    std::string LocalizedGearGameFile(const std::vector<uint8_t>& coalesced);

    struct PlacedChapter { int slot = 0; ChapterDecl decl; std::string mod; };
    struct PlacedCampaign
    {
        std::string id, title, screenshot;
        int act = 0;                                  // ActNum (0-based: the game's acts are 0-4)
        std::vector<PlacedChapter> chapters;          // in N order
    };
    struct Allocation
    {
        std::vector<PlacedCampaign> campaigns;
        std::vector<std::string> problems;            // chapters left out: no free slot, N given twice
        std::vector<std::string> log;                 // one line a campaign: "campaign gears1 'Gears of War': act 6, chapters 32-34"
    };
    constexpr int kFirstModChapter = 32, kLastModChapter = 49;   // EChapterPoint CHAP_DLC_1 .. CHAP_DLC_18
    Allocation AllocateCampaigns(const std::vector<Manifest>& mods, const UsedSlots& used);

    // the allocation as a config fragment (MergeConfig syntax): [ActN GearCampaignActData] / [ChapterN
    // GearCampaignChapterData] in Xbox360-GearGame.ini, their names in `localized` (LocalizedGearGameFile). Empty with
    // no campaigns.
    std::string CampaignFragment(const Allocation& a, const std::string& localized = "GearGame.int");

    // "VI" for act 5 (acts count from 0; the menu shows them from I)
    std::string ActNumeral(int act);
}
