// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/format.h"

namespace cutefish::backup {

// The tool's settings, in ~/.config/cutefish/backup.conf. The window and the
// CLI read and write the same file, so a backup made from either goes by the
// same choices. The dialect is the desktop one: groups in brackets, keys
// below, one value after "=". Comma-separated lists, without spaces kept.
class Config
{
public:
    // ~/.config/cutefish/backup.conf; the parent directories are made.
    static std::string defaultPath();
    // A settings file at an explicit path, for the CLI's --config.
    explicit Config(std::string path);

    void load();
    bool save();

    // The folders a backup copies. The home directory when nothing was
    // chosen yet.
    std::vector<std::string> sources() const;
    void setSources(const std::vector<std::string>& sources);

    std::string destination() const;
    void setDestination(const std::string& path);

    // The compression preset's id, "xz" by default.
    std::string compression() const;
    void setCompression(const std::string& id);
    std::optional<CompressionPreset> compressionPreset() const;

    bool systemMode() const;
    void setSystemMode(bool system);

    std::vector<std::string> excludePatterns() const;
    void setExcludePatterns(const std::vector<std::string>& patterns);
    // The user's own rules joined with the defaults a backup always applies.
    std::vector<std::string> effectiveExcludes(bool system) const;

    // The schedule: a systemd user timer the CLI installs.
    bool scheduleEnabled() const;
    void setScheduleEnabled(bool enabled);
    std::string scheduleFrequency() const; // "daily", "weekly", "monthly"
    void setScheduleFrequency(const std::string& frequency);
    int keepCount() const; // archives kept, 0 for all
    void setKeepCount(int count);

    // The last backup that finished, as the overview page shows it.
    std::string lastBackupName() const;
    int64_t lastBackupTime() const;
    void setLastBackup(const std::string& archiveName, int64_t epochSeconds);

private:
    static std::vector<std::string> splitList(const std::string& value);
    static std::string joinList(const std::vector<std::string>& value);

    std::string m_path;
    std::vector<std::pair<std::string, std::string>> m_lines;
};

} // namespace cutefish::backup
