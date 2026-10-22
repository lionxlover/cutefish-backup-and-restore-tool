// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "excluderules.h"

#include <fnmatch.h>

namespace cutefish::backup {

std::vector<std::string> ExcludeRules::homeDefaults()
{
    // Caches and the trash grow and change constantly; a backup that holds
    // them fills the disk with files no one will ever restore. The patterns
    // work by name anywhere below a source, so a ".cache" in a project
    // directory is kept out too.
    return {
        ".cache/",
        "Cache/",
        "Caches/",
        ".thumbnails/",
        ".temp/",
        "temp/",
        "tmp/",
        ".Trash/",
        ".Trash-*/",
        "*.tmp",
        "*.temp",
        "*.swp",
        "*.bak",
        "*~",
        "node_modules/",
        "target/",
        "venv/",
        ".venv/",
        "__pycache__/",
        ".pytest_cache/",
        "*.pyc",
    };
}

std::vector<std::string> ExcludeRules::systemDefaults()
{
    // Kernel interfaces and the mounts of other disks: files that are not
    // files, and the backup's own destination, which it must not swallow.
    // The walk enters none of them, which also keeps the /proc scan from
    // hanging on threads.
    return {
        "/proc",
        "/sys",
        "/dev",
        "/run",
        "/tmp",
        "/mnt",
        "/media",
        "/var/tmp",
        "/var/cache",
        "/proc/*",
        "/sys/*",
        "/dev/*",
        "/run/*",
    };
}

ExcludeRules::ExcludeRules(std::vector<std::string> patterns)
    : m_patterns(std::move(patterns))
{
}

static bool nameMatches(const std::string& pattern, const std::string& name)
{
    return ::fnmatch(pattern.c_str(), name.c_str(), 0) == 0;
}

static bool pathMatches(const std::string& pattern, const std::string& path)
{
    return ::fnmatch(pattern.c_str(), path.c_str(), FNM_PATHNAME) == 0;
}

bool ExcludeRules::matches(const std::string& path, const std::string& name) const
{
    for (const std::string& raw : m_patterns) {
        std::string pattern = raw;
        // A directory rule also takes the files inside.
        const bool directory = pattern.size() >= 2 && pattern.back() == '/';
        if (directory)
            pattern.pop_back();
        if (pattern.empty())
            continue;
        if (pattern.front() == '/') {
            // A path rule, against the file's path below its source.
            std::string fixed = pattern.substr(1);
            if (pathMatches(fixed, path))
                return true;
            if (directory) {
                // "Documents/" also matches everything inside.
                if (path.size() > fixed.size() && path.compare(0, fixed.size(), fixed) == 0
                    && path[fixed.size()] == '/')
                    return true;
            }
        } else {
            // A name rule, anywhere.
            if (nameMatches(pattern, name))
                return true;
            if (directory) {
                if (pathMatches(pattern, path))
                    return true;
                size_t at = path.find(pattern);
                while (at != std::string::npos) {
                    const bool starts = at == 0 || path[at - 1] == '/';
                    const size_t after = at + pattern.size();
                    const bool ends = after == path.size() || path[after] == '/';
                    if (starts && ends)
                        return true;
                    at = path.find(pattern, at + 1);
                }
            }
        }
    }
    return false;
}

bool ExcludeRules::excludesDirectory(const std::string& path) const
{
    // The name of a directory is the last part of its path.
    const size_t slash = path.rfind('/');
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (name.empty())
        return false;
    if (matches(path, name))
        return true;
    // A rule may name a directory below: "Documents/Reports" excludes
    // "Documents" the same way, as nothing below matches without it.
    for (const std::string& raw : m_patterns) {
        std::string pattern = raw;
        const bool directory = pattern.size() >= 2 && pattern.back() == '/';
        if (directory)
            pattern.pop_back();
        if (pattern.empty())
            continue;
        if (pattern.front() == '/') {
            pattern = pattern.substr(1);
        } else {
            // "Documents/Reports", without a leading slash, is a name rule
            // on its last part; the parent matches through that.
            const size_t inner = pattern.rfind('/');
            if (inner == std::string::npos)
                continue;
            pattern = pattern.substr(0, inner);
            // "*/Reports/x" keeps its stars: only plain names are joined.
            if (pattern.find('*') != std::string::npos || pattern.find('?') != std::string::npos)
                continue;
            const size_t outer = path.rfind('/');
            const std::string parent = outer == std::string::npos ? std::string() : path.substr(0, outer);
            if (parent.empty() || pathMatches(pattern, parent) || nameMatches(pattern, parent))
                return true;
            continue;
        }
        // A directory below a named path is excluded with it.
        if (path.size() > pattern.size() && path.compare(0, pattern.size(), pattern) == 0
            && path[pattern.size()] == '/')
            return true;
    }
    return false;
}

} // namespace cutefish::backup
