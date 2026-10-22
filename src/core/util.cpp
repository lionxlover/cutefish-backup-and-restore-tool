// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "util.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <limits.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

namespace cutefish::backup {

bool isDirectory(const std::string& path)
{
    if (path.empty())
        return false;
    struct stat st;
    if (::stat(path.c_str(), &st) != 0)
        return false;
    return S_ISDIR(st.st_mode);
}

bool isFile(const std::string& path)
{
    struct stat st;
    if (::stat(path.c_str(), &st) != 0)
        return false;
    return S_ISREG(st.st_mode);
}

bool fileExists(const std::string& path)
{
    return ::access(path.c_str(), F_OK) == 0;
}

bool makeDirectories(const std::string& path)
{
    if (path.empty() || path == "." || isDirectory(path))
        return true;
    const std::string parent = directoryOf(path);
    if (!parent.empty() && parent != path && !makeDirectories(parent))
        return false;
    return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

bool removeFile(const std::string& path)
{
    return ::unlink(path.c_str()) == 0 || ::rmdir(path.c_str()) == 0;
}

std::string directoryOf(const std::string& path)
{
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos)
        return ".";
    if (slash == 0)
        return "/";
    return path.substr(0, slash);
}

std::string baseName(const std::string& path)
{
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos)
        return path;
    return path.substr(slash + 1);
}

std::string humanReadableSize(uint64_t bytes)
{
    static const char* units[] = {"B", "kB", "MB", "GB", "TB"};
    double value = double(bytes);
    int unit = 0;
    while (value >= 1000.0 && unit < 4) {
        value /= 1000.0;
        ++unit;
    }
    char text[48];
    if (unit == 0)
        std::snprintf(text, sizeof(text), "%u B", unsigned(bytes));
    else if (value >= 100.0)
        std::snprintf(text, sizeof(text), "%.0f %s", value, units[unit]);
    else
        std::snprintf(text, sizeof(text), "%.1f %s", value, units[unit]);
    return text;
}

uint64_t directorySize(const std::string& path)
{
    uint64_t total = 0;
    // A manual walk: a depth first recursion through opendir keeps control of
    // the order and of errors, which std::filesystem would throw on.
    std::function<void(const std::string&)> walk = [&](const std::string& directory) {
        DIR* dir = ::opendir(directory.c_str());
        if (!dir)
            return;
        while (dirent* entry = ::readdir(dir)) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..")
                continue;
            const std::string full = directory == "/" ? "/" + name : directory + "/" + name;
            struct stat st;
            if (::lstat(full.c_str(), &st) != 0)
                continue;
            if (S_ISDIR(st.st_mode))
                walk(full);
            else if (S_ISREG(st.st_mode))
                total += uint64_t(st.st_size);
        }
        ::closedir(dir);
    };
    walk(normalizePath(path));
    return total;
}

bool readFile(const std::string& path, std::string& into)
{
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    into.clear();
    char buffer[65536];
    size_t read;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        into.append(buffer, read);
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    return ok;
}

bool writeFile(const std::string& path, const std::string& data)
{
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file)
        return false;
    const bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), file) == data.size();
    std::fclose(file);
    return ok;
}

std::string absolutePath(const std::string& path)
{
    if (!path.empty() && path.front() == '/')
        return normalizePath(path);
    char current[PATH_MAX];
    if (!::getcwd(current, sizeof(current)))
        return normalizePath("/" + path);
    return normalizePath(std::string(current) + "/" + path);
}

std::string normalizePath(const std::string& path)
{
    std::string result = path;
    while (result.size() > 1 && result.back() == '/')
        result.pop_back();
    if (result.empty())
        result = "/";
    return result;
}

std::string timeStamp(bool utc)
{
    std::time_t now = std::time(nullptr);
    std::tm parts;
    if (utc)
        parts = *std::gmtime(&now);
    else
        parts = *std::localtime(&now);
    char text[32];
    std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &parts);
    return text;
}

std::string hostName()
{
    char name[256] = {0};
    if (::gethostname(name, sizeof(name) - 1) != 0)
        return "?";
    return name;
}

bool runningAsRoot()
{
    return ::geteuid() == 0;
}

} // namespace cutefish::backup
