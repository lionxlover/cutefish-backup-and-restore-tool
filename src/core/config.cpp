// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "config.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "excluderules.h"
#include "util.h"

namespace cutefish::backup {

static const char* const kGeneral = "General";
static const char* const kExcludes = "Excludes";
static const char* const kSchedule = "Schedule";

static std::string homeDirectory()
{
    if (const char* home = ::getenv("HOME"); home && *home)
        return home;
    if (const passwd* entry = ::getpwuid(::geteuid()); entry && entry->pw_dir && *entry->pw_dir)
        return entry->pw_dir;
    return "/";
}

std::string Config::defaultPath()
{
    return homeDirectory() + "/.config/cutefish/backup.conf";
}

Config::Config(std::string path)
    : m_path(std::move(path))
{
}

std::vector<std::string> Config::splitList(const std::string& value)
{
    std::vector<std::string> parts;
    size_t at = 0;
    while (at <= value.size()) {
        size_t comma = value.find(',', at);
        if (comma == std::string::npos)
            comma = value.size();
        if (comma > at)
            parts.push_back(value.substr(at, comma - at));
        at = comma + 1;
    }
    return parts;
}

std::string Config::joinList(const std::vector<std::string>& value)
{
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (i)
            out += ',';
        out += value[i];
    }
    return out;
}

void Config::load()
{
    m_lines.clear();
    std::string text;
    if (!readFile(m_path, text))
        return;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty() || line.front() == '#' || line.front() == ';')
            continue;
        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            m_lines.push_back({line, std::string()});
            continue;
        }
        std::string key = line.substr(0, equals);
        std::string value = line.substr(equals + 1);
        while (!key.empty() && key.back() == ' ')
            key.pop_back();
        while (!value.empty() && value.front() == ' ')
            value.erase(value.begin());
        m_lines.push_back({key, value});
    }
}

static std::string groupKey(const char* group, const char* key)
{
    return std::string("[") + group + "] " + key;
}

// The lines are flat; a key is found by its group.
static const std::string* find(const std::vector<std::pair<std::string, std::string>>& lines,
                               const char* group, const char* key)
{
    const std::string wanted = groupKey(group, key);
    std::string groupHeader = std::string("[") + group + "]";
    std::string within;
    for (const auto& [name, value] : lines) {
        if (!name.empty() && name.front() == '[') {
            within = name;
            continue;
        }
        if (within == groupHeader && name == key)
            return &value;
    }
    (void)wanted;
    return nullptr;
}

static void assign(std::vector<std::pair<std::string, std::string>>& lines, const char* group,
                   const char* key, const std::string& value)
{
    std::string groupHeader = std::string("[") + group + "]";
    ssize_t groupAt = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!lines[i].first.empty() && lines[i].first.front() == '[') {
            if (lines[i].first == groupHeader)
                groupAt = ssize_t(i);
            continue;
        }
        if (groupAt >= 0 && lines[i].first == key) {
            lines[i].second = value;
            return;
        }
        if (groupAt < 0 && lines[i].first == key) {
            // A key before any group is [General]'s.
            if (std::string(group) == kGeneral) {
                lines[i].second = value;
                return;
            }
        }
    }
    if (groupAt < 0) {
        lines.push_back({groupHeader, std::string()});
        groupAt = ssize_t(lines.size()) - 1;
    }
    lines.insert(lines.begin() + size_t(groupAt) + 1, {std::string(key), value});
}

bool Config::save()
{
    makeDirectories(directoryOf(m_path));
    std::string text;
    std::string group;
    for (const auto& [name, value] : m_lines) {
        text += name;
        if (!name.empty() && name.front() != '[')
            text += "=" + value;
        text += '\n';
    }
    return writeFile(m_path, text);
}

std::vector<std::string> Config::sources() const
{
    if (const std::string* value = find(m_lines, kGeneral, "sources"))
        return splitList(*value);
    return {homeDirectory()};
}

void Config::setSources(const std::vector<std::string>& sources)
{
    std::vector<std::string> cleaned;
    for (const std::string& source : sources) {
        if (source.empty())
            continue;
        const std::string path = normalizePath(source);
        if (std::find(cleaned.begin(), cleaned.end(), path) == cleaned.end())
            cleaned.push_back(path);
    }
    assign(m_lines, kGeneral, "sources", joinList(cleaned));
}

std::string Config::destination() const
{
    if (const std::string* value = find(m_lines, kGeneral, "destination"))
        return *value;
    return homeDirectory() + "/Backups";
}

void Config::setDestination(const std::string& path)
{
    assign(m_lines, kGeneral, "destination", normalizePath(path));
}

std::string Config::compression() const
{
    if (const std::string* value = find(m_lines, kGeneral, "compression"))
        return *value;
    return "xz";
}

void Config::setCompression(const std::string& id)
{
    assign(m_lines, kGeneral, "compression", id);
}

std::optional<CompressionPreset> Config::compressionPreset() const
{
    return presetFromId(compression());
}

bool Config::systemMode() const
{
    if (const std::string* value = find(m_lines, kGeneral, "systemMode"))
        return *value == "true" || *value == "1";
    return false;
}

void Config::setSystemMode(bool system)
{
    assign(m_lines, kGeneral, "systemMode", system ? "true" : "false");
}

std::vector<std::string> Config::excludePatterns() const
{
    if (const std::string* value = find(m_lines, kExcludes, "patterns"))
        return splitList(*value);
    return {};
}

void Config::setExcludePatterns(const std::vector<std::string>& patterns)
{
    assign(m_lines, kExcludes, "patterns", joinList(patterns));
}

std::vector<std::string> Config::effectiveExcludes(bool system) const
{
    std::vector<std::string> all = system ? ExcludeRules::systemDefaults() : ExcludeRules::homeDefaults();
    for (const std::string& pattern : excludePatterns())
        all.push_back(pattern);
    // A system backup never swallows its own destination.
    if (system)
        all.push_back(destination() + "/");
    return all;
}

bool Config::scheduleEnabled() const
{
    if (const std::string* value = find(m_lines, kSchedule, "enabled"))
        return *value == "true" || *value == "1";
    return false;
}

void Config::setScheduleEnabled(bool enabled)
{
    assign(m_lines, kSchedule, "enabled", enabled ? "true" : "false");
}

std::string Config::scheduleFrequency() const
{
    if (const std::string* value = find(m_lines, kSchedule, "frequency")) {
        if (*value == "daily" || *value == "weekly" || *value == "monthly")
            return *value;
    }
    return "daily";
}

void Config::setScheduleFrequency(const std::string& frequency)
{
    assign(m_lines, kSchedule, "frequency", frequency);
}

int Config::keepCount() const
{
    if (const std::string* value = find(m_lines, kSchedule, "keep")) {
        const int count = std::atoi(value->c_str());
        if (count >= 0)
            return count;
    }
    return 10;
}

void Config::setKeepCount(int count)
{
    assign(m_lines, kSchedule, "keep", std::to_string(count));
}

std::string Config::lastBackupName() const
{
    if (const std::string* value = find(m_lines, kSchedule, "lastName"))
        return *value;
    return {};
}

int64_t Config::lastBackupTime() const
{
    if (const std::string* value = find(m_lines, kSchedule, "lastTime"))
        return std::strtoll(value->c_str(), nullptr, 10);
    return 0;
}

void Config::setLastBackup(const std::string& archiveName, int64_t epochSeconds)
{
    assign(m_lines, kSchedule, "lastName", archiveName);
    assign(m_lines, kSchedule, "lastTime", std::to_string(epochSeconds));
}

} // namespace cutefish::backup
