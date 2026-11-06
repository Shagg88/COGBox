// CogBox 0.4.6 (Shagg 6 Nov, the player icon): the pictures in a game's default.xex - its XDBF (SPA) resource's image
// section: the gamer pictures (Marcus, Dom ...), the title image and the achievement icons, all PNG.
// Reads the XEX2 the way Gears1-Native/tools/xextool/xextool.py does (retail AES key, then devkit; "basic" or
// "normal" (LZX) compression). No SDL, no Dear ImGui. Our code (MIT).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xdbf {

struct Image {
    uint64_t id;        // XDBF image id: an achievement's image_id, 0x8000 the title image, 0x1xxxx / 0x2xxxx gamer pictures
    std::string png;
};

// Every PNG in the title's XDBF image section. False (and *err) when the file is not an XEX2 this reader knows, or
// has no XDBF. title_id: the XEX's title ID (0 if not known).
bool read_xex_images(const std::filesystem::path &xex, std::vector<Image> &out, uint32_t *title_id, std::string *err);
// The same from the file's bytes.
bool parse_xex_images(const std::string &file, std::vector<Image> &out, uint32_t *title_id, std::string *err);
// The images of one XDBF blob.
bool parse_xdbf_images(const uint8_t *d, size_t n, std::vector<Image> &out);

}  // namespace xdbf
