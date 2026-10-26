// Layer 0 (game-free): optional dump of the loaded game image for offline analysis (function-pointer table scan,
// tools/scan_vtables.py). Off unless --gow2_dump_image=<file>. The dump is game-derived: never commit it.
#include <xenos/dev_tools.h>
#include <rex/cvar.h>
#include <rex/runtime.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

REXCVAR_DEFINE_STRING(gow2_dump_image, "", "Gears2",
                      "Write the loaded game image (guest 0x82000000, SizeOfImage from its PE header) to this file");

void XenosDumpImageIfRequested(rex::Runtime* runtime) {
  const std::string path = REXCVAR_GET(gow2_dump_image);
  if (path.empty() || runtime == nullptr) return;
  const uint8_t* image = runtime->virtual_membase() + 0x82000000u;
  uint32_t size = 0;
  if (image[0] == 'M' && image[1] == 'Z') {
    uint32_t pe = 0;
    std::memcpy(&pe, image + 0x3C, 4);  // PE headers are little-endian
    if (pe > 0 && pe < 0x1000 && std::memcmp(image + pe, "PE\0\0", 4) == 0) {
      std::memcpy(&size, image + pe + 24 + 56, 4);  // OptionalHeader.SizeOfImage
    }
  }
  if (size == 0 || size > 0x08000000u) {
    std::fprintf(stderr, "gow2_dump_image: no valid PE header at 0x82000000, nothing written\n");
    return;
  }
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr) return;
  std::fwrite(image, 1, size, f);
  std::fclose(f);
  std::fprintf(stderr, "gow2_dump_image: wrote %u bytes to %s\n", size, path.c_str());
}
