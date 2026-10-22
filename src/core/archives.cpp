// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "archives.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <set>

#include <archive.h>
#include <archive_entry.h>

#include "format.h"
#include "util.h"

namespace cutefish::backup {

// Whether a name ends in `tail`, whatever the case of the letters is not.
static bool endsWithName(const std::string& name, const char* tail)
{
    const size_t length = std::strlen(tail);
    return name.size() >= length && name.compare(name.size() - length, length, tail) == 0;
}

// What a file is: a Portable Backup by its own header — the codec is
// written inside, not spelled in the name — or one of the older plain
// compressed tars, whose endings told the codec. Empty for no archive.
static std::string formatOfArchive(const std::string& path, const std::string& name)
{
    if (endsWithName(name, ".pb")) {
        pb::Header header;
        bool isPb = false;
        if (pb::readFileHeader(path, header, &isPb, nullptr) && isPb)
            return pb::codecText(header.codec);
        return "pb";
    }
    if (endsWithName(name, ".tar.zst"))
        return "zstd";
    if (endsWithName(name, ".tar.xz"))
        return "xz";
    if (endsWithName(name, ".tar.gz"))
        return "gzip";
    if (endsWithName(name, ".tar"))
        return "tar";
    return {};
}

static bool isArchiveName(const std::string& name)
{
    return endsWithName(name, ".pb") || endsWithName(name, ".tar.zst") || endsWithName(name, ".tar.xz")
        || endsWithName(name, ".tar.gz") || endsWithName(name, ".tar");
}

std::vector<ArchiveInfo> listArchives(const std::string& directory)
{
    std::vector<ArchiveInfo> list;
    DIR* dir = ::opendir(directory.c_str());
    if (!dir)
        return list;
    while (dirent* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (!isArchiveName(name))
            continue;
        ArchiveInfo info;
        info.name = name;
        info.path = normalizePath(directory) + "/" + name;
        info.format = formatOfArchive(info.path, name);
        struct stat st;
        if (::stat(info.path.c_str(), &st) == 0) {
            info.compressedSize = uint64_t(st.st_size);
            info.modified = st.st_mtime;
        }
        // The files beside the archive are the cache; the .pb file carries
        // the same two in its own trailer, and either answers as well.
        const bool indexedSidecar = fileExists(info.path + ".index");
        const bool metaSidecar = fileExists(info.path + ".meta");
        const bool trailer = pb::hasTrailer(info.path);
        info.hasIndex = indexedSidecar || trailer;
        info.hasMeta = metaSidecar || trailer;
        if (metaSidecar)
            info.meta = readMetaFile(info.path + ".meta");
        if (!info.meta && trailer) {
            pb::Trailer trailerData;
            if (pb::readTrailer(info.path, trailerData) && !trailerData.meta.empty()) {
                Json meta;
                if (Json::parse(trailerData.meta, meta) && meta.isObject())
                    info.meta = meta;
            }
        }
        if (info.meta) {
            info.originalSize = info.meta->get("bytes").toUnsigned();
            info.files = info.meta->get("files").toUnsigned();
        }
        list.push_back(std::move(info));
    }
    ::closedir(dir);
    std::sort(list.begin(), list.end(),
              [](const ArchiveInfo& first, const ArchiveInfo& second) { return first.modified > second.modified; });
    return list;
}

std::optional<Json> readMetaFile(const std::string& path)
{
    std::string text;
    if (!readFile(path, text))
        return std::nullopt;
    Json meta;
    if (!Json::parse(text, meta) || !meta.isObject())
        return std::nullopt;
    return meta;
}

// The index of an archive, wherever it is: the file beside it when the
// backup wrote one, its own trailer otherwise — a Portable Backup taken
// anywhere without its sidecars still knows its own tree. False when
// neither is there.
static bool readIndexText(const std::string& archivePath, std::string& into)
{
    if (readFile(archivePath + ".index", into) && !into.empty())
        return true;
    into.clear();
    pb::Trailer trailer;
    if (pb::readTrailer(archivePath, trailer) && !trailer.index.empty()) {
        into = trailer.index;
        return true;
    }
    return false;
}

bool archiveHasIndex(const std::string& archivePath)
{
    std::string text;
    return readIndexText(archivePath, text);
}

IndexSummary readIndexSummary(const std::string& archivePath)
{
    IndexSummary summary;
    std::string text;
    if (!readIndexText(archivePath, text))
        return summary;
    // The index is one line an entry: type, size, mtime, path. The summary
    // counts the files, folders included in the bytes but not the count.
    uint64_t files = 0;
    uint64_t bytes = 0;
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = text.find('\n', at);
        const size_t stop = end == std::string::npos ? text.size() : end;
        const size_t tab = text.find('\t', at);
        if (tab != std::string::npos && tab < stop) {
            const size_t tab2 = std::min(text.find('\t', tab + 1), stop);
            if (tab2 != stop) {
                if (text[at] != 'd')
                    ++files;
                bytes += uint64_t(std::strtoull(text.substr(tab + 1, tab2 - tab - 1).c_str(), nullptr, 10));
            }
        }
        at = stop + 1;
    }
    summary.valid = true;
    summary.files = files;
    summary.bytes = bytes;
    return summary;
}

static ArchiveEntry parseIndexLine(const std::string& line)
{
    // type, size, mtime, path, each after a tab.
    ArchiveEntry entry;
    const size_t tab = line.find('\t');
    if (tab == std::string::npos)
        return entry;
    entry.type = line.front();
    const size_t tab2 = line.find('\t', tab + 1);
    if (tab2 == std::string::npos)
        return entry;
    const size_t tab3 = line.find('\t', tab2 + 1);
    if (tab3 == std::string::npos)
        return entry;
    entry.size = uint64_t(std::strtoull(line.substr(tab + 1, tab2 - tab - 1).c_str(), nullptr, 10));
    entry.modified = std::strtoll(line.substr(tab2 + 1, tab3 - tab2 - 1).c_str(), nullptr, 10);
    entry.path = line.substr(tab3 + 1);
    entry.name = baseName(entry.path);
    return entry;
}

// The folder an entry's remaining path begins with, when the entry is one
// level deeper than the one listed: a level the archive does not hold an
// entry of its own for, shown anyway so the tree is navigable from the root.
static void addLevelEntry(std::vector<ArchiveEntry>& found, std::set<std::string>& seen,
                          const std::string& below, const std::string& rest)
{
    const size_t slash = rest.find('/');
    if (slash == std::string::npos)
        return;
    const std::string name = rest.substr(0, slash);
    if (name.empty() || seen.find(name) != seen.end())
        return;
    seen.insert(name);
    ArchiveEntry level;
    level.type = 'd';
    level.path = below.empty() ? name : below + "/" + name;
    level.name = name;
    found.push_back(level);
}

bool browseIndex(const std::string& archivePath, const std::string& below, std::vector<ArchiveEntry>& into,
                std::string* error)
{
    std::string text;
    if (!readIndexText(archivePath, text)) {
        if (error)
            *error = "The archive's index is missing.";
        return false;
    }
    std::vector<ArchiveEntry> found;
    std::set<std::string> seen;
    size_t at = 0;
    const std::string prefix = below.empty() ? std::string() : below + "/";
    while (at < text.size()) {
        const size_t end = text.find('\n', at);
        const size_t stop = end == std::string::npos ? text.size() : end;
        const ArchiveEntry entry = parseIndexLine(text.substr(at, stop - at));
        at = stop + 1;
        if (entry.path.empty() || entry.path == below)
            continue;
        if (!below.empty() && entry.path.compare(0, prefix.size(), prefix) != 0)
            continue;
        // Only the level itself: anything deeper shows as the folder it is
        // under, whether the archive holds an entry of it or not.
        const std::string rest = below.empty() ? entry.path : entry.path.substr(prefix.size());
        if (rest.find('/') != std::string::npos) {
            addLevelEntry(found, seen, below, rest);
            continue;
        }
        seen.insert(rest);
        found.push_back(entry);
    }
    std::sort(found.begin(), found.end(), [](const ArchiveEntry& first, const ArchiveEntry& second) {
        // Folders first, then names.
        const bool firstDir = first.type == 'd';
        const bool secondDir = second.type == 'd';
        if (firstDir != secondDir)
            return firstDir;
        return first.name < second.name;
    });
    into = std::move(found);
    return true;
}

bool browseArchiveByReading(const std::string& archivePath, const std::string& below,
                            std::vector<ArchiveEntry>& into, std::string* error)
{
    archive* a = pb::openArchiveForReading(archivePath, error);
    if (!a) {
        if (error && error->empty())
            *error = "The archive could not be opened.";
        return false;
    }
    const std::string prefix = below.empty() ? std::string() : below + "/";
    std::vector<ArchiveEntry> found;
    std::set<std::string> seen;
    archive_entry* entry = nullptr;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        std::string path = archive_entry_pathname(entry);
        while (path.size() >= 2 && path.compare(0, 2, "./") == 0)
            path.erase(0, 2);
        if (path.empty() || path == below)
            continue;
        // tar names folders with a trailing slash; the listing does not.
        while (path.size() >= 2 && path.back() == '/')
            path.pop_back();
        if (!below.empty() && path.compare(0, prefix.size(), prefix) != 0)
            continue;
        const std::string rest = below.empty() ? path : path.substr(prefix.size());
        if (rest.find('/') != std::string::npos) {
            // A level the archive holds no entry of its own for.
            addLevelEntry(found, seen, below, rest);
            continue;
        }
        if (rest.empty() || seen.find(rest) != seen.end())
            continue;
        seen.insert(rest);
        ArchiveEntry one;
        one.type = archive_entry_filetype(entry) == AE_IFDIR
                       ? 'd'
                       : (archive_entry_hardlink(entry) ? 'h' : 'f');
        one.size = uint64_t(archive_entry_size(entry));
        one.modified = archive_entry_mtime(entry);
        one.path = path;
        one.name = rest;
        found.push_back(std::move(one));
    }
    archive_read_free(a);
    std::sort(found.begin(), found.end(), [](const ArchiveEntry& first, const ArchiveEntry& second) {
        const bool firstDir = first.type == 'd';
        const bool secondDir = second.type == 'd';
        if (firstDir != secondDir)
            return firstDir;
        return first.name < second.name;
    });
    into = std::move(found);
    return true;
}

std::vector<std::string> pruneOldArchives(const std::string& directory, int keep)
{
    std::vector<std::string> removed;
    if (keep <= 0)
        return removed;
    const std::vector<ArchiveInfo> archives = listArchives(directory);
    if (int(archives.size()) <= keep)
        return removed;
    for (size_t i = size_t(keep); i < archives.size(); ++i) {
        removeFile(archives[i].path);
        removeFile(archives[i].path + ".index");
        removeFile(archives[i].path + ".meta");
        removed.push_back(archives[i].name);
    }
    return removed;
}

} // namespace cutefish::backup
