// COGBox - .7z reading through the LZMA SDK's 7z decoder (see archive7z.h).
#include "archive7z.h"

#include <string.h>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "../third_party/lzma/7z.h"
#include "../third_party/lzma/7zAlloc.h"
#include "../third_party/lzma/7zCrc.h"
#include "../third_party/lzma/7zFile.h"

static const ISzAlloc g_alloc = { SzAlloc, SzFree }, g_allocTemp = { SzAllocTemp, SzFreeTemp };
static const size_t kInBuf = 1 << 18;
static const UInt64 kMaxBlock = 512ull << 20;   // the largest solid block unpacked in memory

struct Archive7z::Impl {
    CFileInStream file;
    CLookToRead2 look;
    CSzArEx db;
    bool fileOpen = false, dbOpen = false;
    UInt32 blockIndex = 0xFFFFFFFF;
    Byte *outBuffer = NULL;
    size_t outBufferSize = 0;
};

Archive7z::Archive7z() : p_(new Impl) {}
Archive7z::~Archive7z() { close(); delete p_; }

static const char *sz_error(SRes r)
{
    switch (r) {
    case SZ_ERROR_DATA: return "damaged data";
    case SZ_ERROR_MEM: return "not enough memory";
    case SZ_ERROR_CRC: return "CRC mismatch";
    case SZ_ERROR_UNSUPPORTED: return "unsupported compression method";
    case SZ_ERROR_ARCHIVE: return "not a valid 7z archive";
    case SZ_ERROR_NO_ARCHIVE: return "not a 7z archive";
    case SZ_ERROR_INPUT_EOF: return "archive cut short";
    case SZ_ERROR_READ: return "read error";
    default: return "7z error";
    }
}

bool Archive7z::open(const std::string &path)
{
    close();
    err_.clear();
    static bool crcReady = false;
    if (!crcReady) { CrcGenerateTable(); crcReady = true; }
    WRes w;
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, NULL, 0);
    std::vector<wchar_t> wp(n > 0 ? n : 1);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wp.data(), n);
    w = InFile_OpenW(&p_->file.file, wp.data());
#else
    w = InFile_Open(&p_->file.file, path.c_str());
#endif
    if (w != 0) { err_ = "can't open the file"; return false; }
    p_->fileOpen = true;
    FileInStream_CreateVTable(&p_->file);
    p_->file.wres = 0;
    LookToRead2_CreateVTable(&p_->look, False);
    p_->look.buf = (Byte *)ISzAlloc_Alloc(&g_alloc, kInBuf);
    if (!p_->look.buf) { err_ = "not enough memory"; close(); return false; }
    p_->look.bufSize = kInBuf;
    p_->look.realStream = &p_->file.vt;
    LookToRead2_INIT(&p_->look)
    SzArEx_Init(&p_->db);
    p_->dbOpen = true;
    SRes r = SzArEx_Open(&p_->db, &p_->look.vt, &g_alloc, &g_allocTemp);
    if (r != SZ_OK) { err_ = sz_error(r); close(); return false; }
    return true;
}

void Archive7z::close()
{
    if (p_->outBuffer) { ISzAlloc_Free(&g_alloc, p_->outBuffer); p_->outBuffer = NULL; }
    p_->outBufferSize = 0;
    p_->blockIndex = 0xFFFFFFFF;
    if (p_->dbOpen) { SzArEx_Free(&p_->db, &g_alloc); p_->dbOpen = false; }
    if (p_->look.buf && p_->fileOpen) { ISzAlloc_Free(&g_alloc, p_->look.buf); }
    p_->look.buf = NULL;
    if (p_->fileOpen) { File_Close(&p_->file.file); p_->fileOpen = false; }
}

unsigned Archive7z::count() const { return p_->dbOpen ? p_->db.NumFiles : 0; }
bool Archive7z::is_dir(unsigned i) const { return i < count() && SzArEx_IsDir(&p_->db, i); }
unsigned long long Archive7z::size(unsigned i) const { return i < count() ? SzArEx_GetFileSize(&p_->db, i) : 0; }

std::string Archive7z::name(unsigned i) const
{
    if (i >= count()) return "";
    size_t len = SzArEx_GetFileNameUtf16(&p_->db, i, NULL);
    std::vector<UInt16> u(len + 1);
    SzArEx_GetFileNameUtf16(&p_->db, i, u.data());
    std::string out;
    for (size_t k = 0; k < len && u[k]; k++) {
        unsigned c = u[k];
        if (c >= 0xD800 && c < 0xDC00 && k + 1 < len && u[k + 1] >= 0xDC00 && u[k + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (u[k + 1] - 0xDC00);
            k++;
        }
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 63)); out += (char)(0x80 | (c & 63)); }
        else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 63)); out += (char)(0x80 | ((c >> 6) & 63)); out += (char)(0x80 | (c & 63)); }
    }
    return out;
}

bool Archive7z::extract(unsigned i, const unsigned char **data, size_t *size)
{
    *data = NULL;
    *size = 0;
    if (i >= count() || is_dir(i)) { err_ = "not a file"; return false; }
    // 0.4.9 review: a solid block is unpacked whole in memory. Releases are packed with -ms=64m (package_cogbox.ps1);
    // a block over kMaxBlock (another packer's archive, a damaged header) is refused with a clear message instead of
    // asking for gigabytes.
    const UInt32 folder = p_->db.FileToFolder ? p_->db.FileToFolder[i] : (UInt32)-1;
    if (folder != (UInt32)-1 && folder != p_->blockIndex) {
        const UInt64 need = SzAr_GetFolderUnpackSize(&p_->db.db, folder);
        if (need > kMaxBlock) {
            err_ = "this .7z has a solid block of " + std::to_string((unsigned long long)(need >> 20)) +
                   " MB - more than COGBox unpacks in memory (" + std::to_string((unsigned long long)(kMaxBlock >> 20)) +
                   " MB). Repack it with solid blocks of 64 MB (7z -ms=64m) or use the .zip";
            return false;
        }
    }
    size_t offset = 0, processed = 0;
    SRes r = SzArEx_Extract(&p_->db, &p_->look.vt, i, &p_->blockIndex, &p_->outBuffer, &p_->outBufferSize, &offset, &processed,
                            &g_alloc, &g_allocTemp);
    if (r != SZ_OK) { err_ = sz_error(r); return false; }
    *data = p_->outBuffer + offset;
    *size = processed;
    return true;
}
