// COGBox - reading .7z archives (releases from 0.4.9 on are solid LZMA2 .7z) with Igor Pavlov's LZMA SDK 23.01 C decoder
// (public domain, third_party/lzma; licenses/LZMA-SDK-public-domain.txt). No 7z.exe is run.
// The SDK unpacks one solid block at a time into memory, so memory use = the biggest block's unpacked size (a release
// packed with -ms=on keeps most files in one or two blocks; the launcher is 64-bit). Every file's CRC is checked.
#pragma once
#include <cstddef>
#include <string>

class Archive7z {
public:
    Archive7z();
    ~Archive7z();
    // path: UTF-8. false (and error() says why) if it isn't a 7z archive this decoder can read.
    bool open(const std::string &path);
    void close();
    unsigned count() const;
    std::string name(unsigned i) const;   // UTF-8, '/' between folders, as stored
    bool is_dir(unsigned i) const;
    unsigned long long size(unsigned i) const;
    // the file's bytes (valid until the next extract/close); false on a damaged archive, a CRC mismatch or no memory
    bool extract(unsigned i, const unsigned char **data, size_t *size);
    const std::string &error() const { return err_; }
private:
    struct Impl;
    Impl *p_;
    std::string err_;
};
