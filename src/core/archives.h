// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/json.h"

namespace cutefish::backup {

// One archive at the destination. A backup is one .pb file — the tool's own
// container, whose trailer carries its own index and details — and the two
// files made beside it, a .index the restore browser reads without opening
// the archive and a .meta that tells what the machine was, are only a cache
// of what the .pb holds itself.
struct ArchiveInfo
{
    std::string path;
    std::string name;
    uint64_t compressedSize = 0;
    uint64_t originalSize = 0;
    uint64_t files = 0;
    int64_t modified = 0;
    std::string format; // "xz", "zstd" or "gzip", or "tar" for a plain one
    bool hasIndex = false;
    bool hasMeta = false;
    std::optional<Json> meta;
};

// Every archive below the folder, newest first. An archive is a Portable
// Backup — a name ending in .pb — or one of the older plain compressed
// tars, whose endings told the codec: .tar.zst, .tar.xz, .tar.gz or .tar.
std::vector<ArchiveInfo> listArchives(const std::string& directory);

std::optional<Json> readMetaFile(const std::string& path);

// The index a backup wrote — beside the archive, or its own trailer — and
// the counts of it the progress bar wants: one line an entry, the type,
// the size, the time and the path, tab separated. The summary is the whole
// of it read in.
struct IndexSummary
{
    bool valid = false;
    uint64_t files = 0;
    uint64_t bytes = 0;
};
IndexSummary readIndexSummary(const std::string& archivePath);

// Whether an archive can be browsed without reading it through: an index
// beside it, or one in its own trailer.
bool archiveHasIndex(const std::string& archivePath);

// One entry the browser shows.
struct ArchiveEntry
{
    char type = 'f'; // f file, d folder, l link, h hard link
    uint64_t size = 0;
    int64_t modified = 0;
    std::string path; // below the archive
    std::string name;
};

// The entries directly below `below`, from the index — the file beside the
// archive, or the one in its own trailer; a vector is a whole level at
// once. The archive's own root is "". Empty and false on error.
bool browseIndex(const std::string& archivePath, const std::string& below, std::vector<ArchiveEntry>& into,
                 std::string* error);

// The archive's root, read from the archive itself when the index is gone.
// A big archive takes as long to list as to read, so the window says so.
bool browseArchiveByReading(const std::string& archivePath, const std::string& below,
                            std::vector<ArchiveEntry>& into, std::string* error);

// Keeps the newest `keep`, when keep is not zero, and takes the rest away
// with their index and meta. The names of the ones removed come back.
std::vector<std::string> pruneOldArchives(const std::string& directory, int keep);

} // namespace cutefish::backup
