// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <string>
#include <vector>

namespace cutefish::backup {

// Which files a backup leaves out. A pattern is matched the way the GNOME
// backup tools match theirs, so a rule list reads the same in both:
//
//   "*.o"          every file whose name ends in .o, anywhere
//   "Documents/"   the directory Documents below any source, and everything in it
//   "/home/user/.cache"  the directory by its path, and everything in it
//   "*.tmp"        by name, anywhere
//
// A pattern with a leading slash is matched against the path below its
// source; a trailing slash makes it a directory rule, which also matches
// everything inside; anything else is matched against the file's name alone.
// Matching is fnmatch's, so "*" and "?" and "[...]" all work.
class ExcludeRules
{
public:
    // The rules to keep out of a home backup, applied before the user's own.
    static std::vector<std::string> homeDefaults();
    // Applied before everything else when the whole system is backed up.
    static std::vector<std::string> systemDefaults();

    explicit ExcludeRules(std::vector<std::string> patterns);
    // True when `path` is excluded. `path` is the file's path below its
    // source, without a leading slash; `name` is its own name.
    bool matches(const std::string& path, const std::string& name) const;
    // Whether a directory below a source is excluded outright, in which case
    // the walk need not enter it. The files inside match through `matches`
    // too; this is the same test made before the walk.
    bool excludesDirectory(const std::string& path) const;

    const std::vector<std::string>& patterns() const { return m_patterns; }

private:
    std::vector<std::string> m_patterns;
};

} // namespace cutefish::backup
