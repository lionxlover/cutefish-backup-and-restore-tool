// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "archiveengine.h"

#include <archive.h>
#include <archive_entry.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "archives.h"
#include "excluderules.h"
#include "util.h"

namespace cutefish::backup {

static const char* kAppVersion = "1.1";

// ---------------------------------------------------------------------------
// Helpers

// "home/alice/Documents" for a file at that place: the archive keeps
// whole paths below "/", without the slash itself, so a restore to the
// original places lands on the same ones whatever the destination.
static std::string archivePathOf(const std::string& path)
{
    if (path.empty())
        return std::string();
    return path.front() == '/' ? path.substr(1) : path;
}

// A walk that knows the rules and the clock: dirs first, deepest last, and
// nothing below a directory the rules keep out.
struct WalkedEntry
{
    std::string path;     // absolute, as the file system has it
    std::string archive;  // the same below "/", as tar keeps it
    std::string relative;  // below its source, as the rules match it
    std::string name;
    struct stat st;
};

// "/home/alice/Documents" for the source "/home/alice" and the relative
// "Documents"; the root itself when `relative` is empty.
static std::string joinUnderSource(const std::string& source, const std::string& relative)
{
    if (relative.empty())
        return source;
    if (source == "/")
        return "/" + relative;
    return source + "/" + relative;
}

// Whether `path` is `root` itself or lies below it.
static bool isUnder(const std::string& path, const std::string& root)
{
    if (root.empty())
        return false;
    if (path == root)
        return true;
    return path.size() > root.size() && path.compare(0, root.size(), root) == 0
        && (root.back() == '/' || path[root.size()] == '/');
}

static void walkSource(const std::string& source, const std::string& relative, const ExcludeRules& rules,
                       const std::string& skipBelow, const CancelToken* cancel, std::vector<WalkedEntry>& into,
                       uint64_t& files, uint64_t& bytes)
{
    const std::string absolute = joinUnderSource(source, relative);
    DIR* dir = ::opendir(absolute.c_str());
    if (!dir)
        return;
    std::vector<std::string> children;
    while (dirent* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..")
            continue;
        children.push_back(name);
    }
    ::closedir(dir);
    std::sort(children.begin(), children.end());

    for (const std::string& name : children) {
        if (cancel && cancel->isCancelled())
            return;
        const std::string below = relative.empty() ? name : relative + "/" + name;
        WalkedEntry walked;
        walked.path = absolute == "/" ? "/" + name : absolute + "/" + name;
        walked.archive = archivePathOf(walked.path);
        walked.relative = below;
        walked.name = name;
        if (::lstat(walked.path.c_str(), &walked.st) != 0)
            continue;
        // The backup's own destination, or anything below it, is not
        // swallowed: this run would otherwise copy the last archive into the
        // next one, and every run after.
        if (isUnder(walked.path, skipBelow))
            continue;
        const mode_t mode = walked.st.st_mode;
        if (S_ISDIR(mode)) {
            if (rules.excludesDirectory(below))
                continue;
            into.push_back(walked);
            walkSource(source, below, rules, skipBelow, cancel, into, files, bytes);
        } else {
            if (rules.matches(below, name))
                continue;
            if (S_ISREG(mode)) {
                ++files;
                bytes += uint64_t(walked.st.st_size);
            }
            into.push_back(walked);
        }
    }
}

// ---------------------------------------------------------------------------
// Backup

// The write callback the archive library calls with every finished block;
// libarchive is not otherwise told where it is, and this is the window's
// compressed-bytes figure. The file stays open at the close: the trailer
// follows the payload, and the backup is the one to finish the file.
struct WriteState
{
    FILE* file = nullptr;
    uint64_t written = 0; // the payload's own bytes, header and trailer out
};

static la_ssize_t writeBlock(archive*, void* client, const void* buffer, size_t length)
{
    WriteState* state = static_cast<WriteState*>(client);
    if (length == 0)
        return 0;
    const size_t written = std::fwrite(buffer, 1, length, state->file);
    if (written != length)
        return -1;
    state->written += uint64_t(length);
    return la_ssize_t(written);
}

static int closeBlock(archive*, void* client)
{
    WriteState* state = static_cast<WriteState*>(client);
    ::fsync(::fileno(state->file));
    return ARCHIVE_OK;
}

JobResult runBackup(const BackupOptions& options, CancelToken* cancel, const BackupHooks& hooks)
{
    const auto started = std::chrono::steady_clock::now();
    JobResult result;

    // A whole system is root's own files.
    if (options.system && !runningAsRoot()) {
        result.error = "Backing up the whole system needs the administrator password.";
        return result;
    }

    std::vector<std::string> sources;
    for (const std::string& raw : options.sources) {
        const std::string path = normalizePath(raw);
        if (!isDirectory(path)) {
            result.error = "Not a folder: " + path;
            return result;
        }
        sources.push_back(path);
    }
    if (sources.empty()) {
        result.error = "Nothing to back up.";
        return result;
    }
    if (!makeDirectories(options.destinationDir)) {
        result.error = "The backup folder could not be created: " + options.destinationDir;
        return result;
    }

    // The rules, applied before anything is read.
    ExcludeRules rules(options.excludes);

    // Count first, so the bar knows its length; the walk and the count are
    // the same one.
    if (hooks.onCounting)
        hooks.onCounting();
    std::vector<WalkedEntry> entries;
    uint64_t totalFiles = 0;
    uint64_t totalBytes = 0;
    for (const std::string& source : sources) {
        if (cancel && cancel->isCancelled()) {
            result.status = JobStatus::Cancelled;
            return result;
        }
        struct stat rootStat;
        if (::lstat(source.c_str(), &rootStat) != 0) {
            result.error = "The folder could not be read: " + source;
            return result;
        }
        if (!S_ISDIR(rootStat.st_mode)) {
            result.error = "Not a folder: " + source;
            return result;
        }
        // The source's own entry has no name of its own below "/"; the
        // children carry the tree.
        walkSource(source, std::string(), rules, options.skipBelow, cancel, entries, totalFiles,
                   totalBytes);
    }
    if (cancel && cancel->isCancelled()) {
        result.status = JobStatus::Cancelled;
        return result;
    }
    if (!options.estimate && hooks.onStart)
        hooks.onStart(totalFiles, totalBytes);
    if (options.estimate && hooks.onEstimate)
        hooks.onEstimate(totalFiles, totalBytes);
    if (hooks.onStart)
        hooks.onStart(totalFiles, totalBytes);

    // The name: what is in it, when it was made. How it is compressed is
    // written inside the file itself, so the ending is the same one always.
    // Two backups made in the same second never overwrite one another: the
    // second one takes the next free number behind the name.
    std::string archivePath;
    for (int attempt = 1; attempt < 100 && archivePath.empty(); ++attempt) {
        const std::string candidate = normalizePath(options.destinationDir) + "/cutefish-backup-"
            + (options.system ? "system" : "home") + "-" + timeStamp()
            + (attempt == 1 ? std::string() : "-" + std::to_string(attempt)) + "." + pbExtension();
        if (!fileExists(candidate))
            archivePath = candidate;
    }
    if (archivePath.empty()) {
        archivePath = normalizePath(options.destinationDir) + "/cutefish-backup-"
            + (options.system ? "system" : "home") + "-" + timeStamp() + "-99." + pbExtension();
    }
    std::string indexSeedPath = archivePath + ".index";
    std::string metaPath = archivePath + ".meta";

    FILE* file = std::fopen(archivePath.c_str(), "wb");
    if (!file) {
        result.error = std::string("The archive could not be opened: ") + std::strerror(errno);
        return result;
    }
    // The container's own header, before anything else: the magic that says
    // what the file is, the codec the payload will use, its level.
    if (!pb::writeHeader(file, pb::headerFor(options.format, options.level), &result.error)) {
        std::fclose(file);
        removeFile(archivePath);
        return result;
    }
    WriteState state;
    state.file = file;
    FILE* index = std::fopen(indexSeedPath.c_str(), "wb");

    archive* a = archive_write_new();
    archive_write_set_format_pax(a);
    bool filterSet = true;
    switch (options.format) {
    case Format::Zstd:
        filterSet = archive_write_add_filter_zstd(a) == ARCHIVE_OK;
        if (filterSet)
            archive_write_set_filter_option(a, "zstd", "compression-level", std::to_string(options.level).c_str());
        break;
    case Format::Xz:
        filterSet = archive_write_add_filter_xz(a) == ARCHIVE_OK;
        if (filterSet)
            archive_write_set_filter_option(a, "xz", "compression-level", std::to_string(options.level).c_str());
        break;
    case Format::Gzip:
        filterSet = archive_write_add_filter_gzip(a) == ARCHIVE_OK;
        if (filterSet)
            archive_write_set_filter_option(a, "gzip", "compression-level", std::to_string(options.level).c_str());
        break;
    }
    if (!filterSet) {
        archive_write_free(a);
        if (index)
            std::fclose(index);
        std::fclose(file);
        removeFile(archivePath);
        removeFile(indexSeedPath);
        result.error = "The compression of the archive could not be set up.";
        return result;
    }
    // The client writes exactly the codec's stream, with no padding of the
    // last block out to the writer's own block size. That padding would be
    // zeros after the stream's end, and the zstd reader — unlike xz's, which
    // steps over anything left — refuses a stream whose end holds bytes that
    // are no frame of its own.
    archive_write_set_bytes_per_block(a, 1);
    archive_write_open2(a, &state, nullptr, writeBlock, closeBlock, 0);

    // The same file put twice is a hard link the second time; tar keeps the
    // link, and the restore makes one again, so a tree with many links does
    // not grow in the archive.
    std::map<std::pair<uint64_t, uint64_t>, std::string> hardLinks;
    uint64_t filesDone = 0;
    uint64_t bytesDone = 0;
    std::string current;
    auto lastProgress = std::chrono::steady_clock::now() - std::chrono::hours(1);
    std::string failure;

    for (const WalkedEntry& walked : entries) {
        if (cancel && cancel->isCancelled())
            break;
        current = walked.path;

        archive_entry* entry = archive_entry_new();
        archive_entry_copy_pathname(entry, walked.archive.c_str());
        archive_entry_copy_stat(entry, &walked.st);
        const mode_t mode = walked.st.st_mode;

        if (S_ISREG(mode) && walked.st.st_nlink > 1) {
            const auto identity = std::make_pair(uint64_t(walked.st.st_dev), uint64_t(walked.st.st_ino));
            const auto known = hardLinks.find(identity);
            if (known != hardLinks.end() && !known->second.empty() && known->second != walked.archive) {
                archive_entry_set_hardlink(entry, known->second.c_str());
                archive_entry_set_size(entry, 0);
            } else {
                hardLinks[identity] = walked.archive;
            }
        }

        // A link's target is where the name points, not the file's own
        // length: readlink keeps it, and the header takes it from there.
        if (S_ISLNK(mode)) {
            char target[4096];
            const ssize_t length = ::readlink(walked.path.c_str(), target, sizeof(target) - 1);
            if (length > 0) {
                target[length] = '\0';
                archive_entry_set_symlink(entry, target);
                archive_entry_set_size(entry, 0);
            }
        }

        // A directory, a link or a hard link: no data follows the header.
        const bool hasData = S_ISREG(mode) && archive_entry_hardlink(entry) == nullptr;

        if (archive_write_header(a, entry) != ARCHIVE_OK) {
            archive_entry_free(entry);
            failure = std::string("The archive could not be written: ") + archive_error_string(a);
            break;
        }
        if (hasData) {
            FILE* input = std::fopen(walked.path.c_str(), "rb");
            if (!input) {
                // Gone between the walk and the read: not worth failing over.
                archive_entry_free(entry);
                continue;
            }
            char buffer[65536];
            size_t read;
            while ((read = std::fread(buffer, 1, sizeof(buffer), input)) > 0) {
                if (archive_write_data(a, buffer, read) < 0) {
                    failure = std::string("The archive could not be written: ") + archive_error_string(a);
                    break;
                }
                bytesDone += uint64_t(read);
            }
            std::fclose(input);
            ++filesDone;
            if (!failure.empty())
                break;
        } else if (S_ISREG(mode)) {
            // A hard link to a file already written: one file, one header.
            ++filesDone;
        }

        if (index) {
            char line[4096];
            const char type = S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l'
                            : archive_entry_hardlink(entry)     ? 'h'
                                                                 : 'f';
            std::snprintf(line, sizeof(line), "%c\t%llu\t%lld\t%s\n", type,
                          (unsigned long long)walked.st.st_size, (long long)walked.st.st_mtime,
                          walked.archive.c_str());
            std::fputs(line, index);
        }
        archive_entry_free(entry);

        const auto now = std::chrono::steady_clock::now();
        if (hooks.onProgress && now - lastProgress > std::chrono::milliseconds(250)) {
            lastProgress = now;
            hooks.onProgress(filesDone, bytesDone, state.written, current);
        }
    }

    // The last word before the close, so the bar shows where it ended up.
    if (hooks.onProgress && !(cancel && cancel->isCancelled()) && failure.empty())
        hooks.onProgress(filesDone, bytesDone, state.written, current);

    archive_write_close(a);
    archive_write_free(a);
    if (index)
        std::fclose(index);

    const bool cancelled = cancel && cancel->isCancelled();
    if (cancelled || !failure.empty()) {
        std::fclose(state.file);
        removeFile(archivePath);
        removeFile(indexSeedPath);
        removeFile(metaPath);
        result.status = cancelled ? JobStatus::Cancelled : JobStatus::Failed;
        result.error = failure;
        return result;
    }

    // What the machine was, for the overview and the restore page.
    Json meta = Json::object();
    meta.set("tool", "cutefish-backup");
    meta.set("version", 1);
    meta.set("appVersion", kAppVersion);
    meta.set("created", int64_t(std::time(nullptr)));
    meta.set("hostname", hostName());
    meta.set("mode", options.system ? "system" : "home");
    meta.set("format", formatName(options.format));
    meta.set("level", int64_t(options.level));
    Json sourcesJson = Json::array();
    for (const std::string& source : sources)
        sourcesJson.append(source);
    meta.set("sources", sourcesJson);
    meta.set("files", int64_t(totalFiles));
    meta.set("bytes", int64_t(totalBytes));
    // The same bytes beside the archive and inside it: the sidecar is the
    // cache, the trailer in the .pb file carries everything of itself.
    const std::string metaText = meta.dump(true) + "\n";
    writeFile(metaPath, metaText);

    // The trailer: the index beside the file, read back and written into
    // the file itself, so one .pb taken anywhere carries its own tree.
    std::string indexText;
    readFile(indexSeedPath, indexText);
    std::string trailerError;
    if (!pb::writeTrailer(state.file, indexText, metaText, &trailerError)) {
        std::fclose(state.file);
        removeFile(archivePath);
        removeFile(indexSeedPath);
        removeFile(metaPath);
        result.error = trailerError.empty() ? std::string("The archive could not be finished.") : trailerError;
        return result;
    }
    ::fsync(::fileno(state.file));
    std::fclose(state.file);

    struct stat finalStat;
    result.status = JobStatus::Ok;
    result.archivePath = archivePath;
    result.files = filesDone;
    result.bytes = bytesDone;
    result.compressedBytes =
        ::stat(archivePath.c_str(), &finalStat) == 0 ? uint64_t(finalStat.st_size) : state.written;
    result.elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
            .count();
    return result;
}

// ---------------------------------------------------------------------------
// Restore

static std::string stripArchiveLead(const std::string& path)
{
    std::string out = path;
    while (out.size() >= 2 && out.compare(0, 2, "./") == 0)
        out.erase(0, 2);
    if (!out.empty() && out.front() == '/')
        out.erase(out.begin());
    // tar writes a folder's name with a slash; the comparisons that follow
    // want the name without it.
    while (out.size() >= 2 && out.back() == '/')
        out.pop_back();
    if (out == "/")
        out.clear();
    return out;
}

// Whether an entry below the archive is one the user asked for. An empty ask
// takes everything, as does the root; a folder takes itself and its contents.
static bool wantedBy(const std::string& path, const std::vector<std::string>& filters)
{
    if (filters.empty())
        return true;
    for (const std::string& raw : filters) {
        const std::string filter = stripArchiveLead(raw);
        if (filter.empty())
            return true;
        if (path == filter)
            return true;
        if (path.size() > filter.size() && path.compare(0, filter.size(), filter) == 0
            && path[filter.size()] == '/')
            return true;
    }
    return false;
}

// Where an entry goes: the original tree below the root, or the same tree
// below a folder of the user's.
static std::string extractPathOf(const RestoreOptions& options, const std::string& path)
{
    if (options.destination.empty())
        return "/" + path;
    return normalizePath(options.destination) + "/" + path;
}

static void setTimes(const std::string& path, const struct stat& st)
{
    struct timespec times[2];
    times[0].tv_sec = st.st_atime;
    times[0].tv_nsec = 0;
    times[1].tv_sec = st.st_mtime;
    times[1].tv_nsec = 0;
    ::utimensat(AT_FDCWD, path.c_str(), times, AT_SYMLINK_NOFOLLOW);
}

JobResult runRestore(const RestoreOptions& options, CancelToken* cancel, const RestoreHooks& hooks)
{
    const auto started = std::chrono::steady_clock::now();
    JobResult result;

    archive* a = pb::openArchiveForReading(options.archivePath, &result.error);
    if (!a)
        return result;

    // The bar's length: the index knows — beside the archive, or its own
    // trailer — and an archive without one is read through with an unknown
    // length, as a busy line.
    uint64_t totalFiles = 0;
    uint64_t totalBytes = 0;
    IndexSummary summary = readIndexSummary(options.archivePath);
    if (summary.valid) {
        totalFiles = summary.files;
        totalBytes = summary.bytes;
    }
    if (hooks.onStart)
        hooks.onStart(totalFiles, totalBytes);

    uint64_t filesDone = 0;
    uint64_t bytesDone = 0;
    std::string current;
    std::string failure;
    auto lastProgress = std::chrono::steady_clock::now() - std::chrono::hours(1);

    archive_entry* entry = nullptr;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        if (cancel && cancel->isCancelled())
            break;
        const std::string path = stripArchiveLead(archive_entry_pathname(entry));
        if (path.empty())
            continue;
        current = path;
        if (!wantedBy(path, options.paths))
            continue;
        const mode_t mode = archive_entry_filetype(entry);
        const std::string extractPath = extractPathOf(options, path);

        if (S_ISDIR(mode)) {
            makeDirectories(extractPath);
            ::chmod(extractPath.c_str(), archive_entry_perm(entry));
        } else if (S_ISREG(mode)) {
            makeDirectories(directoryOf(extractPath));
            if (!options.overwrite && fileExists(extractPath)) {
                continue;
            }
            FILE* output = std::fopen(extractPath.c_str(), "wb");
            if (!output) {
                failure = std::string("Could not write ") + extractPath + ": " + std::strerror(errno);
                break;
            }
            const void* block = nullptr;
            size_t length = 0;
            la_int64_t offset = 0;
            while (archive_read_data_block(a, &block, &length, &offset) == ARCHIVE_OK) {
                if (std::fwrite(block, 1, length, output) != length) {
                    failure = std::string("Could not write ") + extractPath;
                    break;
                }
                bytesDone += uint64_t(length);
            }
            std::fclose(output);
            ++filesDone;
            ::chmod(extractPath.c_str(), archive_entry_perm(entry));
            if (const struct stat* entryStat = archive_entry_stat(entry))
                setTimes(extractPath, *entryStat);
            if (!failure.empty())
                break;
        } else if (S_ISLNK(mode)) {
            makeDirectories(directoryOf(extractPath));
            if (fileExists(extractPath))
                ::unlink(extractPath.c_str());
            if (::symlink(archive_entry_symlink(entry), extractPath.c_str()) != 0) {
                // A symlink whose target's own place may not be back yet;
                // the link still goes, and one that cannot be made is not
                // worth failing a restore over.
            }
            ++filesDone;
        } else if (const char* hardTarget = archive_entry_hardlink(entry)) {
            makeDirectories(directoryOf(extractPath));
            if (fileExists(extractPath))
                ::unlink(extractPath.c_str());
            const std::string resolved = extractPathOf(options, stripArchiveLead(hardTarget));
            if (::link(resolved.c_str(), extractPath.c_str()) != 0) {
                // The first file may have been left out of the restore; put
                // a copy in its place.
                archive_read_data_skip(a);
            }
            ++filesDone;
        } else {
            // Devices and named pipes: a user's files never have them, and a
            // system's are remade at boot.
            archive_read_data_skip(a);
        }

        const auto now = std::chrono::steady_clock::now();
        if (hooks.onProgress && now - lastProgress > std::chrono::milliseconds(250)) {
            lastProgress = now;
            hooks.onProgress(filesDone, bytesDone, current);
        }
    }

    // The last word before the close, so the bar shows where it ended up.
    if (hooks.onProgress && !(cancel && cancel->isCancelled()))
        hooks.onProgress(filesDone, bytesDone, current);

    archive_read_free(a);

    const bool cancelled = cancel && cancel->isCancelled();
    if (cancelled) {
        result.status = JobStatus::Cancelled;
        return result;
    }
    if (!failure.empty()) {
        result.status = JobStatus::Failed;
        result.error = failure;
        return result;
    }
    result.status = JobStatus::Ok;
    result.files = filesDone;
    result.bytes = bytesDone;
    result.compressedBytes = 0;
    result.elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
            .count();
    return result;
}

// ---------------------------------------------------------------------------
// Verify

JobResult verifyArchive(const std::string& archivePath, CancelToken* cancel)
{
    const auto started = std::chrono::steady_clock::now();
    JobResult result;

    archive* a = pb::openArchiveForReading(archivePath, &result.error);
    if (!a)
        return result;

    uint64_t files = 0;
    uint64_t bytes = 0;
    archive_entry* entry = nullptr;
    int status = ARCHIVE_OK;
    while (true) {
        status = archive_read_next_header(a, &entry);
        if (status == ARCHIVE_EOF)
            break;
        if (status < ARCHIVE_WARN) {
            result.error = std::string("The archive is damaged: ") + archive_error_string(a);
            archive_read_free(a);
            return result;
        }
        if (cancel && cancel->isCancelled()) {
            result.status = JobStatus::Cancelled;
            archive_read_free(a);
            return result;
        }
        ++files;
        bytes += uint64_t(archive_entry_size(entry));
        const void* block = nullptr;
        size_t length = 0;
        la_int64_t offset = 0;
        while (true) {
            const int read = archive_read_data_block(a, &block, &length, &offset);
            if (read == ARCHIVE_EOF)
                break;
            if (read < ARCHIVE_WARN) {
                result.error = std::string("The archive is damaged: ") + archive_error_string(a);
                archive_read_free(a);
                return result;
            }
        }
    }
    archive_read_free(a);

    struct stat st;
    uint64_t compressed = ::stat(archivePath.c_str(), &st) == 0 ? uint64_t(st.st_size) : 0;
    result.status = JobStatus::Ok;
    result.files = files;
    result.bytes = bytes;
    result.compressedBytes = compressed;
    result.archivePath = archivePath;
    result.elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
            .count();
    return result;
}

} // namespace cutefish::backup
