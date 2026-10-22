// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "format.h"

#include <archive.h>

#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

namespace cutefish::backup {

std::string formatName(Format format)
{
    switch (format) {
    case Format::Zstd:
        return "zstd";
    case Format::Xz:
        return "xz";
    case Format::Gzip:
        return "gzip";
    }
    return "zstd";
}

std::optional<Format> formatFromName(const std::string& name)
{
    if (name == "zstd" || name == "zst")
        return Format::Zstd;
    if (name == "xz" || name == "lzma2")
        return Format::Xz;
    if (name == "gzip" || name == "gz")
        return Format::Gzip;
    return std::nullopt;
}

std::string pbExtension()
{
    // One ending for every codec: the file says its own compression inside.
    return "pb";
}

const std::vector<CompressionPreset>& compressionPresets()
{
    // The default is LZMA2: as the GNOME backup tools keep whole backups in
    // heavily compressed archives, the backup tool trades speed for size and
    // starts with the codec that makes the smallest files.
    static const std::vector<CompressionPreset> presets = {
        {"xz", "xz (LZMA2) — smallest", Format::Xz, 6},
        {"zstd-fast", "zstd — fastest", Format::Zstd, 3},
        {"zstd-high", "zstd — high", Format::Zstd, 19},
        {"gzip", "gzip — compatible", Format::Gzip, 6},
    };
    return presets;
}

std::optional<CompressionPreset> presetFromId(const std::string& id)
{
    for (const CompressionPreset& preset : compressionPresets()) {
        if (id == preset.id)
            return preset;
    }
    return std::nullopt;
}

namespace pb {

// The magic of the container: 'C','P','B' and the version, 0x01. A file
// beginning with it is a Portable Backup; a file without it is read as the
// plain compressed tar it once was.
static const char kMagic[4] = {'C', 'P', 'B', char(0x01)};
// The trailer's footer, which names where the payload stops.
static const char kTrailerMagic[4] = {'C', 'P', 'B', 'T'};

// Every number in the container is little-endian, the one way it writes and
// reads them.
static void store16(uint8_t* at, uint16_t value)
{
    at[0] = uint8_t(value);
    at[1] = uint8_t(value >> 8);
}

static void store32(uint8_t* at, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        at[i] = uint8_t(value >> (8 * i));
}

static void store64(uint8_t* at, uint64_t value)
{
    for (int i = 0; i < 8; ++i)
        at[i] = uint8_t(value >> (8 * i));
}

static uint16_t load16(const uint8_t* at)
{
    return uint16_t(at[0]) | uint16_t(at[1] << 8);
}

static uint32_t load32(const uint8_t* at)
{
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i)
        value |= uint32_t(at[i]) << (8 * i);
    return value;
}

static uint64_t load64(const uint8_t* at)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
        value |= uint64_t(at[i]) << (8 * i);
    return value;
}

Header headerFor(Format format, int level)
{
    Header header;
    switch (format) {
    case Format::Zstd:
        header.codec = CodecZstd;
        break;
    case Format::Xz:
        header.codec = CodecXz;
        break;
    case Format::Gzip:
        header.codec = CodecGzip;
        break;
    }
    header.level = level >= 0 && level <= 255 ? uint8_t(level) : 0;
    header.flags = FlagTrailer; // the backup always writes one
    return header;
}

std::string codecText(uint8_t codec)
{
    switch (codec) {
    case CodecXz:
        return "xz";
    case CodecZstd:
        return "zstd";
    case CodecGzip:
        return "gzip";
    case CodecNone:
        return "tar";
    }
    return "pb";
}

bool writeHeader(FILE* file, const Header& header, std::string* error)
{
    uint8_t bytes[HeaderSize] = {0};
    std::memcpy(bytes, kMagic, 4);
    bytes[4] = header.codec;
    bytes[5] = header.level;
    store16(bytes + 6, header.flags);
    store32(bytes + 8, uint32_t(HeaderSize));
    store32(bytes + 12, 0);
    if (std::fwrite(bytes, 1, HeaderSize, file) != HeaderSize) {
        if (error)
            *error = std::string("The archive's header could not be written: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool readFileHeader(const std::string& path, Header& into, bool* isPb, std::string* error)
{
    if (isPb)
        *isPb = false;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        if (error)
            *error = std::string("The archive could not be opened: ") + std::strerror(errno);
        return false;
    }
    uint8_t bytes[HeaderSize];
    const size_t got = std::fread(bytes, 1, sizeof(bytes), file);
    std::fclose(file);
    if (got != sizeof(bytes)) {
        if (error)
            *error = "The archive could not be opened: too short to be one.";
        return false;
    }
    if (std::memcmp(bytes, kMagic, 4) != 0)
        return true; // a plain compressed tar: no error, and no .pb either
    Header parsed;
    parsed.codec = bytes[4];
    parsed.level = bytes[5];
    parsed.flags = load16(bytes + 6);
    into = parsed;
    if (isPb)
        *isPb = true;
    return true;
}

// The trailer's footer, when the file ends in one: the last twelve bytes
// hold its magic and the length of the body before them. The length must
// fit between the header and the footer and hold at least the counts
// inside it — a frame that does not add up is no footer.
static bool trailerFooter(const std::string& path, uint64_t& trailerLength, uint64_t& fileSize)
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || st.st_size < 0)
        return false;
    const uint64_t size = uint64_t(st.st_size);
    if (size < HeaderSize + 12 + 24)
        return false;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    uint8_t footer[12];
    const bool read = ::fseeko(file, off_t(size - 12), SEEK_SET) == 0
        && std::fread(footer, 1, sizeof(footer), file) == sizeof(footer);
    std::fclose(file);
    if (!read || std::memcmp(footer, kTrailerMagic, 4) != 0)
        return false;
    const uint64_t length = load64(footer + 4);
    if (length < 24 || length > size - HeaderSize - 12)
        return false;
    trailerLength = length;
    fileSize = size;
    return true;
}

bool hasTrailer(const std::string& path)
{
    uint64_t length = 0;
    uint64_t size = 0;
    return trailerFooter(path, length, size);
}

bool readTrailer(const std::string& path, Trailer& into)
{
    uint64_t trailerLength = 0;
    uint64_t fileSize = 0;
    if (!trailerFooter(path, trailerLength, fileSize))
        return false;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    // The size, then the bytes; a size in braces would read as a list of
    // one element instead.
    std::vector<uint8_t> body;
    body.resize(size_t(trailerLength));
    const bool read = ::fseeko(file, off_t(fileSize - 12 - trailerLength), SEEK_SET) == 0
        && std::fread(body.data(), 1, body.size(), file) == body.size();
    std::fclose(file);
    if (!read)
        return false;
    // version(4) + reserved(4) + index length(8) + the index + meta
    // length(4) + the meta: the body is only these, all of it.
    const uint32_t version = load32(body.data());
    if (version != 1)
        return false; // a layout to come: the archive still reads, its
                      // quick index only is not understood
    const uint64_t indexLength = load64(body.data() + 8);
    if (indexLength > body.size() - 24)
        return false;
    const uint64_t rest = body.size() - 24 - indexLength;
    if (rest < 4)
        return false;
    const uint32_t metaLength = load32(body.data() + 24 + size_t(indexLength));
    if (uint64_t(metaLength) + 4 != rest)
        return false; // everything the footer names must be accounted for
    into.index.assign(reinterpret_cast<const char*>(body.data() + 24), size_t(indexLength));
    into.meta.assign(reinterpret_cast<const char*>(body.data() + 24 + size_t(indexLength) + 4),
                     size_t(metaLength));
    return true;
}

bool writeTrailer(FILE* file, const std::string& index, const std::string& meta, std::string* error)
{
    std::vector<uint8_t> body(24 + index.size() + 4 + meta.size());
    store32(body.data(), 1); // the trailer's own version
    store32(body.data() + 4, 0);
    store64(body.data() + 8, uint64_t(index.size()));
    if (!index.empty())
        std::memcpy(body.data() + 24, index.data(), index.size());
    store32(body.data() + 24 + index.size(), uint32_t(meta.size()));
    if (!meta.empty())
        std::memcpy(body.data() + 24 + index.size() + 4, meta.data(), meta.size());
    uint8_t footer[12];
    std::memcpy(footer, kTrailerMagic, 4);
    store64(footer + 4, uint64_t(body.size()));
    const bool written = std::fwrite(body.data(), 1, body.size(), file) == body.size()
        && std::fwrite(footer, 1, sizeof(footer), file) == sizeof(footer);
    if (!written && error)
        *error = std::string("The archive's trailer could not be written: ") + std::strerror(errno);
    return written;
}

bool payloadRange(const std::string& path, uint64_t& start, uint64_t& end)
{
    Header header;
    bool isPb = false;
    if (!readFileHeader(path, header, &isPb, nullptr) || !isPb)
        return false;
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || st.st_size < 0)
        return false;
    start = HeaderSize;
    end = uint64_t(st.st_size);
    uint64_t trailerLength = 0;
    uint64_t size = 0;
    if (trailerFooter(path, trailerLength, size))
        end -= 12 + trailerLength;
    return true;
}

// ---------------------------------------------------------------------------
// Reading: libarchive bound to the payload

// The client libarchive reads through: the payload's own slice of the file,
// so the header never reaches the bidders and the trailer never follows the
// stream's end — a codec stream with a foreign tail would make some readers
// stop with a complaint, and this way they never see it.
struct Slice
{
    FILE* file = nullptr;
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t position = 0; // absolute, within the file
    char buffer[65536];
};

static int sliceOpen(archive*, void* client)
{
    Slice* slice = static_cast<Slice*>(client);
    if (::fseeko(slice->file, off_t(slice->start), SEEK_SET) != 0)
        return ARCHIVE_FATAL;
    slice->position = slice->start;
    return ARCHIVE_OK;
}

static la_ssize_t sliceRead(archive*, void* client, const void** buffer)
{
    Slice* slice = static_cast<Slice*>(client);
    if (slice->position >= slice->end) {
        *buffer = nullptr;
        return 0;
    }
    size_t want = sizeof(slice->buffer);
    if (uint64_t(want) > slice->end - slice->position)
        want = size_t(slice->end - slice->position);
    const size_t got = std::fread(slice->buffer, 1, want, slice->file);
    if (got == 0)
        return -1; // the file ends before its own bounds: short, not slow
    slice->position += uint64_t(got);
    *buffer = slice->buffer;
    return la_ssize_t(got);
}

static la_int64_t sliceSeek(archive*, void* client, la_int64_t request, int whence)
{
    Slice* slice = static_cast<Slice*>(client);
    int64_t target = -1;
    switch (whence) {
    case SEEK_SET:
        target = int64_t(slice->start) + request;
        break;
    case SEEK_CUR:
        target = int64_t(slice->position) + request;
        break;
    case SEEK_END:
        target = int64_t(slice->end) + request;
        break;
    default:
        return -1;
    }
    if (target < int64_t(slice->start))
        return -1;
    if (::fseeko(slice->file, off_t(target), SEEK_SET) != 0)
        return -1;
    slice->position = uint64_t(target);
    return target - int64_t(slice->start);
}

static int sliceClose(archive*, void* client)
{
    Slice* slice = static_cast<Slice*>(client);
    std::fclose(slice->file);
    delete slice;
    return ARCHIVE_OK;
}

archive* openArchiveForReading(const std::string& path, std::string* error)
{
    Header header;
    bool isPb = false;
    std::string headerError;
    if (!readFileHeader(path, header, &isPb, &headerError)) {
        if (error)
            *error = headerError.empty() ? std::string("The archive could not be opened.") : headerError;
        return nullptr;
    }

    if (!isPb) {
        // A plain compressed tar — the tool's own first files, and any other
        // tool's — read whole, the codec told by its own magic bytes.
        archive* a = archive_read_new();
        archive_read_support_format_tar(a);
        archive_read_support_filter_all(a);
        if (archive_read_open_filename(a, path.c_str(), 65536) == ARCHIVE_OK)
            return a;
        if (error)
            *error = std::string("The archive could not be opened: ")
                + (archive_error_string(a) ? archive_error_string(a) : "not readable");
        archive_read_free(a);
        return nullptr;
    }

    uint64_t start = 0;
    uint64_t end = 0;
    if (!payloadRange(path, start, end)) {
        if (error)
            *error = "The archive could not be opened.";
        return nullptr;
    }
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        if (error)
            *error = std::string("The archive could not be opened: ") + std::strerror(errno);
        return nullptr;
    }
    Slice* slice = new Slice;
    slice->file = file;
    slice->start = start;
    slice->end = end;

    archive* a = archive_read_new();
    archive_read_support_format_tar(a);
    archive_read_support_filter_all(a);
    archive_read_set_callback_data(a, slice);
    archive_read_set_open_callback(a, sliceOpen);
    archive_read_set_read_callback(a, sliceRead);
    archive_read_set_seek_callback(a, sliceSeek);
    archive_read_set_close_callback(a, sliceClose);
    if (archive_read_open1(a) == ARCHIVE_OK)
        return a;
    if (error)
        *error = std::string("The archive could not be opened: ")
            + (archive_error_string(a) ? archive_error_string(a) : "unrecognized");
    // The free closes the file and takes the slice with it.
    archive_read_free(a);
    return nullptr;
}

} // namespace pb

} // namespace cutefish::backup
