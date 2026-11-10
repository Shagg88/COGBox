// COGBox - the games' own sound, read from the player's copy for the library's music (music.cpp). Nothing is copied,
// saved or shipped: a package is read into memory, the wanted wave decoded, the rest dropped.
//
// Where the Gears games keep sound (job 986, Shagg's four copies, 1 Nov 2026): no XACT wave banks (.xwb / .xsb) at all -
// every sound is a SoundNodeWave object inside the UE3 packages (.xxx), its Xbox 360 data an XMA stream.
//
// UE3 package on the 360 (big-endian): either "fully compressed" (the whole file is chunks: tag 0x9E2A83C1, block size,
// compressed / uncompressed totals, a (compressed, uncompressed) pair per block, then the blocks: XMemCompress LZX, or
// LZO1X in Gears 1's older packages, which store the tag again in place of the block size), or a plain summary followed
// by such chunks. The summary: tag, version, header size, folder name, flags, name count / offset, export count /
// offset, ...; a name is an FString and 8 bytes of flags; an export holds class, super, outer, name (index, number),
// archetype, flags (8 bytes), serial size, serial offset (version-dependent past that).
//
// A SoundNodeWave's Xbox 360 data is a bulk data record - flags, element count, size on disk, offset in file (4 bytes
// each, big-endian), then the bytes - holding UE3's XMA layout (vgmstream xma_ue3.c; UE Viewer UnSound.h): format
// chunk size, seek table size, data size (u32 BE each), the format chunk (XMA2WAVEFORMAT, big-endian; or a 0x34-byte
// WAVEFORMATEX + XMA2 extension, or an XMA1 XMAWAVEFORMAT - little-endian in Gears 1, job 994), the seek table, then XMA packets (2048 bytes each). The
// three sizes must add up to the record's element count, so a wrong record is never taken for sound.
//
// XMA decoding: FFmpeg's xma1 / xma2 decoder (libavcodec, LGPL 2.1), avcodec-63.dll + avutil-61.dll (FFmpeg 9.0.2) beside CogBox.exe,
// loaded only when music is wanted (missing DLLs: no music, nothing else changes). See build.sh / tools/build_ffmpeg_xma.sh.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gamesound {

// One file of a game copy: 'copy' is a folder (the extracted game, the title update) or a disc image / Games on Demand
// header (read in place through x360fs); 'rel' uses '/'. Up to 'max' bytes; false if it is not there or bigger.
bool read_copy_file(const std::string &copy, const std::string &rel, std::vector<uint8_t> &out, size_t max);

// The package's bytes as the engine sees them (every compressed chunk decoded in place); false if it isn't a package
// or a chunk can't be decoded.
bool ue3_unpack(const std::vector<uint8_t> &file, std::vector<uint8_t> &out, std::string &why);

struct Wave {
    size_t at = 0, size = 0;    // the XMA record (from its format chunk size on) in the unpacked package
    int xma = 2;                // 1: XMA1, 2: XMA2
    bool le = false;            // the format chunk is little-endian (Gears 1's XMA1)
    int channels = 0, rate = 0;
    uint32_t samples = 0;       // as the format says (XMA1: from its bytes per second; 0: not known)
    std::string name;           // the export's name when the summary could be read ("" otherwise)
    double seconds() const { return rate > 0 ? double(samples) / rate : 0; }
};
// Every XMA wave in an unpacked package, in file order.
std::vector<Wave> ue3_find_waves(const std::vector<uint8_t> &pkg);

// FFmpeg's decoder could be loaded (avcodec-63.dll, avutil-61.dll beside CogBox.exe, xma2 decoder present)
bool xma_available(std::string *why = nullptr);
// Decodes one wave to 16-bit stereo at 44.1 kHz. Refused (false, 'why' says why) when the packets don't decode as XMA:
// a wrong record is never played as noise. 'cancel' (optional) is polled between packets.
bool xma_decode(const std::vector<uint8_t> &pkg, const Wave &w, std::vector<int16_t> &stereo44k, std::string &why,
                bool (*cancel)(void *) = nullptr, void *ctx = nullptr);

// What leads a track in (start of the track up to the first sample louder than about -54 dB) is cut.
void trim_leading_silence(std::vector<int16_t> &stereo);

}  // namespace gamesound
