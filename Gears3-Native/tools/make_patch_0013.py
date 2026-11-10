#!/usr/bin/env python3
"""Apply patch 0013 (compare dump) to a copy of Gears2-Native shadow_renderer.cpp.

Usage: make_patch_0013.py <in shadow_renderer.cpp> <out shadow_renderer.cpp>
Each edit is anchored on exact text; the script stops if an anchor is missing or not unique.
"""
import sys

src = open(sys.argv[1], encoding="utf-8", newline="").read()
nl = "\r\n" if "\r\n" in src else "\n"


def edit(old, new):
    global src
    old = old.replace("\n", nl)
    new = new.replace("\n", nl)
    n = src.count(old)
    if n != 1:
        sys.exit(f"anchor found {n} times: {old[:80]!r}")
    src = src.replace(old, new)


# 1. Switches, next to the tile trace switches.
edit('''REXCVAR_DEFINE_STRING(gow2_renderer_tile_trace_pixel,''',
     '''// Gears 3 notes/15 (black squad): on chosen comparison frames, write each compared colour resolve raw to disk - our
// words, the reference's words (same convention), the last-writer draw ID per pixel - plus the frame's draw list, so a
// region that differs (a dark character) can be traced to the draw that wrote it. Diagnostic only; off by default.
REXCVAR_DEFINE_STRING(gow2_renderer_compare_dump_frames, "", "Gears2",
                      "Comparison frames (comma list, e.g. 2700,2850) whose colour resolves are written raw to "
                      "--gow2_renderer_compare_dump_dir: ours, the reference, the last-writer draw ID per pixel and the "
                      "frame's draw list (empty = off)");
REXCVAR_DEFINE_STRING(gow2_renderer_compare_dump_dir, "compare-dump", "Gears2",
                      "Folder for --gow2_renderer_compare_dump_frames (relative = to the working directory)");
REXCVAR_DEFINE_STRING(gow2_renderer_tile_trace_pixel,''')

# 2. Capture buffers before the pixel loop.
edit('''    std::vector<Example> examples, transferred_examples;
    if (ours && !p.guest.empty()) {
      for (uint32_t y = 0; y < h; ++y) {''',
     '''    std::vector<Example> examples, transferred_examples;
    // --gow2_renderer_compare_dump_frames: every pixel's two values and last writer, for an offline diff image.
    std::vector<uint32_t> dump_ours, dump_guest, dump_id;
    if (ours && !p.guest.empty() && DumpFrame(p.frame)) {
      dump_ours.assign(size_t(w) * h * p.words, 0u);
      dump_guest.assign(dump_ours.size(), 0u);
      dump_id.assign(size_t(w) * h, 0u);
    }
    if (ours && !p.guest.empty()) {
      for (uint32_t y = 0; y < h; ++y) {''')

# 3. Record each pixel (equal ones too) right after the word loop.
edit('''              diff = std::max(diff, WordDiff(ours_word, gg[k], p.dest_format));
            }
          }
          if (equal) {
            ++pixels_equal;
            continue;
          }''',
     '''              diff = std::max(diff, WordDiff(ours_word, gg[k], p.dest_format));
            }
          }
          if (!dump_ours.empty()) {
            const size_t i = size_t(y) * w + x;
            for (uint32_t k = 0; k < p.words; ++k) {
              dump_ours[i * p.words + k] = og[k];
              dump_guest[i * p.words + k] = gg[k];
            }
            if (cov) {
              float mf;
              std::memcpy(&mf, cov + size_t(y) * cov_pitch + size_t(x) * 4, 4);
              dump_id[i] = mf >= 1.0f && mf < 16777216.0f ? uint32_t(mf) : 0u;
            }
          }
          if (equal) {
            ++pixels_equal;
            continue;
          }''')

# 4. Write after the summary line.
edit('''                extra, out_of_range ? ", " + std::to_string(out_of_range) + " px outside the snapshot" : std::string(),
                p.not_drawn, verdict);''',
     '''                extra, out_of_range ? ", " + std::to_string(out_of_range) + " px outside the snapshot" : std::string(),
                p.not_drawn, verdict);
    if (!dump_ours.empty()) WriteCompareDump(p, w, h, dump_ours, dump_guest, dump_id, verdict);''')

# 5. Helpers, before CompareColour.
edit('''  void CompareColour(const PendingResolve& p) {''',
     '''  bool DumpFrame(uint64_t frame) {
    if (!dump_parsed_) {
      dump_parsed_ = true;
      const std::string spec = REXCVAR_GET(gow2_renderer_compare_dump_frames);
      size_t pos = 0;
      while (pos < spec.size()) {
        const size_t end = std::min(spec.find(',', pos), spec.size());
        const std::string part = spec.substr(pos, end - pos);
        if (!part.empty()) dump_frames_.insert(std::strtoull(part.c_str(), nullptr, 10));
        pos = end + 1;
      }
    }
    return dump_frames_.count(frame) != 0;
  }

  // Files: f<frame>-c<copy>.ours.bin / .guest.bin (w*h*words little-endian uint32, our word convention for both),
  // .id.bin (w*h uint32: 0 not written this frame, 1 transfer, even N = draw record N/2-1, odd N >= 3 = that record
  // drawn with a texture stand-in), f<frame>-draws.txt (the record list) and index.txt (one line per dumped copy).
  void WriteCompareDump(const PendingResolve& p, uint32_t w, uint32_t h, const std::vector<uint32_t>& ours,
                        const std::vector<uint32_t>& guest, const std::vector<uint32_t>& id, const std::string& verdict) {
    std::error_code ec;
    const std::filesystem::path dir = REXCVAR_GET(gow2_renderer_compare_dump_dir);
    std::filesystem::create_directories(dir, ec);
    char stem[64];
    std::snprintf(stem, sizeof(stem), "f%06llu-c%03u", static_cast<unsigned long long>(p.frame), p.copy);
    auto put = [&](const char* ext, const std::vector<uint32_t>& v) {
      if (FILE* f = std::fopen((dir / (std::string(stem) + ext)).string().c_str(), "wb")) {
        std::fwrite(v.data(), 4, v.size(), f);
        std::fclose(f);
      }
    };
    put(".ours.bin", ours);
    put(".guest.bin", guest);
    put(".id.bin", id);
    if (FILE* f = std::fopen((dir / "index.txt").string().c_str(), "ab")) {
      std::fprintf(f, "%s w %u h %u words %u dest_format %u swap %d draws %u-%u | %s | %s\\n", stem, w, h, p.words,
                   p.dest_format, p.swap ? 1 : 0, p.first_draw, p.last_draw, p.source.c_str(), verdict.c_str());
      std::fclose(f);
    }
    // The frame's record list grows during the frame: rewrite it whenever a later copy sees more records.
    if (p.records && p.records->size() > dump_records_written_[p.frame]) {
      dump_records_written_[p.frame] = p.records->size();
      char name[48];
      std::snprintf(name, sizeof(name), "f%06llu-draws.txt", static_cast<unsigned long long>(p.frame));
      if (FILE* f = std::fopen((dir / name).string().c_str(), "wb")) {
        for (size_t i = 0; i < p.records->size(); ++i) {
          const DrawRecord& r = (*p.records)[i];
          std::fprintf(f,
                       "id %zu draw %u vs %016llX ps %016llX prim %u target %u colour %d mask %X blend %d %u %u %u "
                       "depth func %u write %d stencil write %d func %u ref %02X stand-in %d vs_relative %d\\n",
                       (i + 1) * 2, r.draw_index, static_cast<unsigned long long>(r.vs),
                       static_cast<unsigned long long>(r.ps), r.primitive, r.target, r.colour ? 1 : 0, r.colour_mask,
                       r.colour_blend ? 1 : 0, r.blend_src, r.blend_dst, r.blend_op, r.depth_func, r.depth_write ? 1 : 0,
                       r.stencil_write ? 1 : 0, r.stencil_func, r.stencil_ref, r.stand_in ? 1 : 0, r.vs_relative ? 1 : 0);
        }
        std::fclose(f);
      }
    }
  }

  void CompareColour(const PendingResolve& p) {''')

# 6. Members, next to the tile trace set.
edit('''  std::bitset<::xenos::edram::kTileCount> trace_tiles_;''',
     '''  std::bitset<::xenos::edram::kTileCount> trace_tiles_;
  bool dump_parsed_ = false;                         // --gow2_renderer_compare_dump_frames
  std::set<uint64_t> dump_frames_;
  std::map<uint64_t, size_t> dump_records_written_;''')

open(sys.argv[2], "w", encoding="utf-8", newline="").write(src)
print("ok")
