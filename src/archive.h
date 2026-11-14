#pragma once

// Reads one file out of the game's own .fat/.dat archives (Data_Win32) at run
// time. Nothing from the game is shipped with the mod.

// A DXT1 texture taken from an .xbt file.
struct ArchiveTexture {
    int width, height, mips;
    unsigned char* file;        // whole file, owned; free with ArchiveFree
    const unsigned char* data;  // first mip level, inside `file`
    unsigned long dataSize;     // bytes from `data` to the end of the file
};

// Looks for the file with this name hash (CRC32 of the lower-case path) in
// patch.fat, then worlds\worlds.fat, and loads it if it is a DXT1 .xbt
// texture. Logs what it found. Returns false if not found or not usable.
bool ArchiveLoadTexture(unsigned long nameHash, ArchiveTexture* out);
void ArchiveFree(ArchiveTexture* t);
