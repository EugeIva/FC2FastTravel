#include "archive.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"

namespace {

// Archive index (.fat), version 5: a 16-byte header, then 16-byte entries.
//   +0  name hash
//   +4  uncompressed size << 2 | compression scheme (0 none, 1 LZO1X)
//   +8  offset low 2 bits << 30 | compressed size
//   +12 offset >> 2
const unsigned long kFatMagic = 0x46415432;  // "FAT2"
const unsigned long kFatVersion = 5;

struct Entry {
    unsigned long hash, usize, scheme, csize;
    unsigned long long offset;
};

unsigned long U32(const unsigned char* p) {
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) |
           ((unsigned long)p[3] << 24);
}

// LZO1X decompression with bounds checks. Returns the number of bytes
// written, or -1 on malformed input.
long Lzo1xDecompress(const unsigned char* src, unsigned long srcLen, unsigned char* dst,
                     unsigned long dstLen) {
    const unsigned char* ip = src;
    const unsigned char* const ipEnd = src + srcLen;
    unsigned char* op = dst;
    unsigned char* const opEnd = dst + dstLen;
    unsigned long t;
    const unsigned char* m;

#define NEED_IP(n) if ((unsigned long)(ipEnd - ip) < (unsigned long)(n)) return -1
#define NEED_OP(n) if ((unsigned long)(opEnd - op) < (unsigned long)(n)) return -1
#define COPY_LIT(n) { NEED_IP(n); NEED_OP(n); memcpy(op, ip, (n)); op += (n); ip += (n); }
#define COPY_MATCH(n) { if (m < dst || m >= op) return -1; NEED_OP(n); \
        for (unsigned long k_ = (n); k_; --k_) *op++ = *m++; }

    NEED_IP(1);
    if (*ip > 17) {
        t = (unsigned long)*ip++ - 17;
        if (t < 4) {
            COPY_LIT(t);
            NEED_IP(1);
            t = *ip++;
            goto match;
        }
        COPY_LIT(t);
        goto first_literal_run;
    }

    for (;;) {
        NEED_IP(1);
        t = *ip++;
        if (t >= 16) goto match;
        if (t == 0) {
            NEED_IP(1);
            while (*ip == 0) {
                t += 255;
                ++ip;
                NEED_IP(1);
            }
            t += 15 + *ip++;
        }
        COPY_LIT(t + 3);

    first_literal_run:
        NEED_IP(1);
        t = *ip++;
        if (t >= 16) goto match;
        NEED_IP(1);
        m = op - (1 + 0x0800) - (t >> 2) - ((unsigned long)*ip++ << 2);
        COPY_MATCH(3);
        goto match_done;

        for (;;) {
        match:
            if (t >= 64) {
                NEED_IP(1);
                m = op - 1 - ((t >> 2) & 7) - ((unsigned long)*ip++ << 3);
                t = (t >> 5) - 1;
                COPY_MATCH(t + 2);
            } else if (t >= 32) {
                t &= 31;
                if (t == 0) {
                    NEED_IP(1);
                    while (*ip == 0) {
                        t += 255;
                        ++ip;
                        NEED_IP(1);
                    }
                    t += 31 + *ip++;
                }
                NEED_IP(2);
                m = op - 1 - (ip[0] >> 2) - ((unsigned long)ip[1] << 6);
                ip += 2;
                COPY_MATCH(t + 2);
            } else if (t >= 16) {
                m = op - ((t & 8) << 11);
                t &= 7;
                if (t == 0) {
                    NEED_IP(1);
                    while (*ip == 0) {
                        t += 255;
                        ++ip;
                        NEED_IP(1);
                    }
                    t += 7 + *ip++;
                }
                NEED_IP(2);
                m -= (ip[0] >> 2) + ((unsigned long)ip[1] << 6);
                ip += 2;
                if (m == op) return (long)(op - dst);  // end of stream
                m -= 0x4000;
                COPY_MATCH(t + 2);
            } else {
                NEED_IP(1);
                m = op - 1 - (t >> 2) - ((unsigned long)*ip++ << 2);
                COPY_MATCH(2);
            }

        match_done:
            t = ip[-2] & 3;
            if (t == 0) break;
            COPY_LIT(t);
            NEED_IP(1);
            t = *ip++;
        }
    }
#undef NEED_IP
#undef NEED_OP
#undef COPY_LIT
#undef COPY_MATCH
}

// Finds `hash` in the index file. The entries are sorted by hash, but a
// linear scan of a few megabytes is fast enough and assumes less.
bool FindEntry(const char* fatPath, unsigned long hash, Entry* out) {
    FILE* f = fopen(fatPath, "rb");
    if (!f) return false;
    bool found = false;
    unsigned char head[16];
    if (fread(head, 1, 16, f) == 16 && U32(head) == kFatMagic && U32(head + 4) == kFatVersion) {
        unsigned long count = U32(head + 12);
        unsigned char buf[16 * 256];
        while (count && !found) {
            unsigned long n = count < 256 ? count : 256;
            if (fread(buf, 16, n, f) != n) break;
            for (unsigned long i = 0; i < n; ++i) {
                const unsigned char* e = buf + 16 * i;
                if (U32(e) != hash) continue;
                unsigned long b = U32(e + 4), c = U32(e + 8), d = U32(e + 12);
                out->hash = hash;
                out->usize = b >> 2;
                out->scheme = b & 3;
                out->csize = c & 0x3FFFFFFF;
                out->offset = ((unsigned long long)d << 2) | (c >> 30);
                found = true;
                break;
            }
            count -= n;
        }
    }
    fclose(f);
    return found;
}

unsigned char* ReadEntry(const char* datPath, const Entry& e, unsigned long* size) {
    const unsigned long kMaxSize = 64u * 1024 * 1024;
    if (e.csize == 0 || e.csize > kMaxSize || e.usize > kMaxSize) return nullptr;
    FILE* f = fopen(datPath, "rb");
    if (!f) return nullptr;
    unsigned char* raw = (unsigned char*)malloc(e.csize);
    bool ok = raw && _fseeki64(f, (long long)e.offset, SEEK_SET) == 0 &&
              fread(raw, 1, e.csize, f) == e.csize;
    fclose(f);
    if (!ok) {
        free(raw);
        return nullptr;
    }
    if (e.scheme == 0) {
        *size = e.csize;
        return raw;
    }
    if (e.scheme != 1 || e.usize == 0) {
        free(raw);
        return nullptr;
    }
    unsigned char* out = (unsigned char*)malloc(e.usize);
    long n = out ? Lzo1xDecompress(raw, e.csize, out, e.usize) : -1;
    free(raw);
    if (n != (long)e.usize) {
        free(out);
        return nullptr;
    }
    *size = e.usize;
    return out;
}

// <game>\bin\FarCry2.exe -> <game>\Data_Win32\<name>
bool DataPath(const char* name, char* out, size_t outSize) {
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    char* slash = strrchr(exe, '\\');
    if (!slash) return false;
    *slash = 0;
    slash = strrchr(exe, '\\');
    if (!slash) return false;
    *slash = 0;
    return _snprintf(out, outSize, "%s\\Data_Win32\\%s", exe, name) > 0 &&
           memchr(out, 0, outSize) != nullptr;
}

}  // namespace

bool ArchiveLoadTexture(unsigned long nameHash, ArchiveTexture* out) {
    static const char* const kArchives[] = {"patch", "worlds\\worlds"};
    for (int a = 0; a < 2; ++a) {
        char name[64], fat[MAX_PATH], dat[MAX_PATH];
        _snprintf(name, sizeof(name), "%s.fat", kArchives[a]);
        if (!DataPath(name, fat, sizeof(fat))) continue;
        _snprintf(name, sizeof(name), "%s.dat", kArchives[a]);
        if (!DataPath(name, dat, sizeof(dat))) continue;

        Entry e;
        if (!FindEntry(fat, nameHash, &e)) continue;
        unsigned long size = 0;
        unsigned char* file = ReadEntry(dat, e, &size);
        if (!file) {
            Log("Map image: %08lX found in %s but could not be read", nameHash, kArchives[a]);
            continue;
        }
        // .xbt: "TBX\0", version, header size, ...; then a DDS file.
        bool ok = size > 160 && memcmp(file, "TBX\0", 4) == 0;
        unsigned long hs = ok ? U32(file + 8) : 0;
        ok = ok && hs < size && size - hs > 128 && memcmp(file + hs, "DDS ", 4) == 0 &&
             memcmp(file + hs + 84, "DXT1", 4) == 0;
        if (!ok) {
            Log("Map image: %08lX in %s is not a DXT1 texture", nameHash, kArchives[a]);
            free(file);
            continue;
        }
        out->height = (int)U32(file + hs + 12);
        out->width = (int)U32(file + hs + 16);
        out->mips = (int)U32(file + hs + 28);
        if (out->mips < 1) out->mips = 1;
        out->file = file;
        out->data = file + hs + 128;
        out->dataSize = size - hs - 128;
        Log("Map image: %08lX loaded from %s, %dx%d, %d mip levels", nameHash, kArchives[a],
            out->width, out->height, out->mips);
        return true;
    }
    Log("Map image: %08lX not found in the game archives", nameHash);
    return false;
}

void ArchiveFree(ArchiveTexture* t) {
    free(t->file);
    t->file = nullptr;
    t->data = nullptr;
}
