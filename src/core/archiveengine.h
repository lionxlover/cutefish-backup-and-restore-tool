// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/format.h"

namespace cutefish::backup {

// A backup or a restore checks this between files; the CLI's signal handler
// sets it, and the window's Cancel button reaches it that way through the
// process it started. Safe to read and write from different threads.
class CancelToken
{
public:
    void cancel() { m_cancelled = true; }
    bool isCancelled() const { return m_cancelled; }

private:
    std::atomic<bool> m_cancelled{false};
};

// How a job ended.
enum class JobStatus {
    Ok,
    Cancelled,
    Failed,
};

// What a backup or a restore did.
struct JobResult
{
    JobStatus status = JobStatus::Failed;
    std::string error;
    // The archive a backup wrote, with its path.
    std::string archivePath;
    uint64_t files = 0;
    uint64_t bytes = 0;
    uint64_t compressedBytes = 0;
    int64_t elapsedMs = 0;
};

// A whole system is one tar of "/", which only root may read; a home backup
// is the user's own files, run without privileges.
struct BackupOptions
{
    std::vector<std::string> sources; // absolute directories
    std::string destinationDir;
    // An absolute path the walk never enters: the backup's own destination,
// so an archive below a source — ~/Backups inside ~ — is not swallowed by
// the next one.
    std::string skipBelow;
    Format format = Format::Xz;
    int level = 6;
    std::vector<std::string> excludes;
    bool system = false;
    // Walk the sources first, for the progress bar; on for the window, off
    // when a script wants the archive started at once.
    bool estimate = true;
};

struct BackupHooks
{
    // The sources are being counted. The walk is not fast on big trees; the
    // UI shows its busy indicator while it runs.
    std::function<void()> onCounting;
    // The counts are in: how many files, how many bytes.
    std::function<void(uint64_t files, uint64_t bytes)> onEstimate;
    // The archive is open; the first file is about to be read.
    std::function<void(uint64_t files, uint64_t bytes)> onStart;
    // Throttled to a few a second: files read, source bytes read, the archive
    // so far, and the file being read.
    std::function<void(uint64_t filesDone, uint64_t bytesDone, uint64_t archiveBytes,
                       const std::string& current)>
        onProgress;
};

// Writes one Portable Backup: cutefish-backup-home-20261022-143005.pb, or
// -system- for root's whole disk. The file is the tool's own container —
// a header that names the codec, the compressed tar under it, and a trailer
// carrying its own index and details — with a .index the restore browser
// reads and a .meta for the overview beside it, both only a cache of what
// the .pb file holds itself.
JobResult runBackup(const BackupOptions& options, CancelToken* cancel, const BackupHooks& hooks);

struct RestoreOptions
{
    std::string archivePath;
    // What to put back: archive paths, as the browser shows them, with or
    // without a leading "./"; a folder takes everything inside it. Empty
    // restores everything.
    std::vector<std::string> paths;
    // Empty puts everything back where it came from, which needs root for a
    // system backup; a path puts the archive's whole tree below it.
    std::string destination;
    bool overwrite = true;
};

struct RestoreHooks
{
    std::function<void(uint64_t files, uint64_t bytes)> onStart;
    std::function<void(uint64_t filesDone, uint64_t bytesDone, const std::string& current)> onProgress;
};

JobResult runRestore(const RestoreOptions& options, CancelToken* cancel, const RestoreHooks& hooks);

// Reads an archive end to end, every byte of every entry, and tells whether
// it is whole. files and bytes come back with it.
JobResult verifyArchive(const std::string& archivePath, CancelToken* cancel);

} // namespace cutefish::backup
