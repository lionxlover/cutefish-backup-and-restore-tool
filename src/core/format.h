// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

struct archive;

namespace cutefish::backup {

// The compression of the payload inside a Portable Backup. "xz" is LZMA2,
// the same codec 7z uses; it gives the smallest files. zstd is much faster
// with slightly larger output, and gzip reads everywhere. The payload of
// every backup is a tar under one of these — but the codec a file uses is
// written in the file's own header, not in its name.
enum class Format {
    Zstd,
    Xz,
    Gzip,
};

// How the compression is written on the command line, in the config file and
// in the archive name. "zstd", "xz" or "gzip"; "zstd" for an unknown spelling.
std::string formatName(Format format);
// Parses "zstd", "xz", "gzip" (or "lzma2", an alias of xz); unknown by null.
std::optional<Format> formatFromName(const std::string& name);
// "pb": the one ending every backup takes, whichever codec is inside it.
std::string pbExtension();

// A compression preset the wizard offers: a format and a level, with the
// trade-off its name tells. Levels follow the codec's own scale.
struct CompressionPreset {
    const char* id;
    const char* label;
    Format format;
    int level;
};

// The presets the GUI lists, first the default.
const std::vector<CompressionPreset>& compressionPresets();
std::optional<CompressionPreset> presetFromId(const std::string& id);

// ---------------------------------------------------------------------------
// The Portable Backup container — every backup is one ".pb" file:
//
//   offset  size  what it is
//        0     4  the magic: 'C','P','B', 0x01 — Cutefish Portable Backup,
//                                        container version 1
//        4     1  the codec: 0 xz (LZMA2), 1 zstd, 2 gzip, 3 none
//        5     1  the compression level
//        6     2  flags, little-endian; bit 0: a trailer follows the payload
//        8     4  the header's own length, 16, for the readers to come
//       12     4  reserved, zero
//       16    ...  the payload: the codec's stream of a POSIX PAX tar
//  end - 12    12  the trailer's footer: 'C','P','B','T' + its length, u64
//
// The trailer carries the file-list index and the JSON details — the same
// bytes the .index and .meta files beside a backup hold — so one .pb file
// taken anywhere carries everything of itself. Its frame is read from the
// file's own last bytes and must add up exactly, never from the flags alone.
namespace pb {

enum Codec : uint8_t {
    CodecXz = 0,
    CodecZstd = 1,
    CodecGzip = 2,
    CodecNone = 3,
};
constexpr uint16_t FlagTrailer = 0x0001;
constexpr size_t HeaderSize = 16;

struct Header
{
    uint8_t codec = CodecXz;
    uint8_t level = 0;
    uint16_t flags = 0;
};

struct Trailer
{
    std::string index; // the .index sidecar's own bytes
    std::string meta;  // the .meta sidecar's own bytes
};

// The header a backup starts with: the codec of its payload, its level, and
// the trailer flag, as the backup always writes one.
Header headerFor(Format format, int level);
// "xz", "zstd", "gzip", "tar" — what a codec byte spells; "pb" for a byte
// no version writes.
std::string codecText(uint8_t codec);

// Writes the 16 bytes a .pb file begins with, onto an open file.
bool writeHeader(FILE* file, const Header& header, std::string* error);
// Reads the first bytes, telling whether the file is a Portable Backup at
// all: a file without the magic is a plain compressed tar, as the tool's
// own first versions and any other tool write, and is read whole. False
// only when the file cannot be read at all.
bool readFileHeader(const std::string& path, Header& into, bool* isPb, std::string* error);

// The payload's bounds within a .pb: after the header, before the trailer
// when the file ends in one. False when the file is no .pb.
bool payloadRange(const std::string& path, uint64_t& start, uint64_t& end);

// Appends the trailer to the file the payload already filled. The footer
// names the length of everything before it, so a reader can tell the two
// apart without a guess.
bool writeTrailer(FILE* file, const std::string& index, const std::string& meta, std::string* error);
// Reads the trailer, when the file ends in one; false when it does not. A
// frame whose lengths do not add up exactly is no trailer.
bool readTrailer(const std::string& path, Trailer& into);
// Whether the file ends in a trailer at all.
bool hasTrailer(const std::string& path);

// Opens an archive for libarchive to read through: a Portable Backup by its
// own header and bounds — the codec's stream and nothing else — and a plain
// compressed tar whole, as it always was. The caller frees with
// archive_read_free, which also closes the file. Null when it cannot be
// opened, and error says why.
archive* openArchiveForReading(const std::string& path, std::string* error);

} // namespace pb

} // namespace cutefish::backup
