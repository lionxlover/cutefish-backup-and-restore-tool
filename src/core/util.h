// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cutefish::backup {

// File-system helpers shared by the archive engine, the CLI and the tests.
// Only POSIX calls; the core must build anywhere, with libarchive its only
// dependency.

// True when the path is a directory. An empty path is not.
bool isDirectory(const std::string& path);
// True when the path is a regular file or a symlink to one.
bool isFile(const std::string& path);
bool fileExists(const std::string& path);
// mkdir -p, one component at a time; true when it exists afterwards.
bool makeDirectories(const std::string& path);
// Removes a file, a link or an empty directory.
bool removeFile(const std::string& path);
// The directory of a path, "/" or "" keeping the meaning they have; without
// a slash the directory is ".".
std::string directoryOf(const std::string& path);
std::string baseName(const std::string& path);
// "12.3 MB", "1.2 GB" …, with one figure below GB.
std::string humanReadableSize(uint64_t bytes);
// The size of every file below the directory, following no symlink, in bytes.
// Skips unreadable entries rather than failing.
uint64_t directorySize(const std::string& path);
// Reads a whole file into a string; empty and false when it cannot.
bool readFile(const std::string& path, std::string& into);
bool writeFile(const std::string& path, const std::string& data);
// "/home/user/Backups" for a relative name, as the tool is built on.
std::string absolutePath(const std::string& path);
// The path without a trailing slash, "/" staying "/".
std::string normalizePath(const std::string& path);
// "20261022-143005", the archive name's time stamp, in UTC or in the local
// zone. The local one by default, as the user knows it.
std::string timeStamp(bool utc = false);
// The host's name, as gethostname gives it, "?" when it does not answer.
std::string hostName();
// The calling user's name, from the environment, "root" for uid 0.
bool runningAsRoot();

} // namespace cutefish::backup
