// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "destinations.h"

#include <algorithm>
#include <cstdio>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include "util.h"

namespace cutefish::backup {

static std::string homeDirectory()
{
    if (const char* home = ::getenv("HOME"); home && *home)
        return home;
    if (const passwd* entry = ::getpwuid(::geteuid()); entry && entry->pw_dir && *entry->pw_dir)
        return entry->pw_dir;
    return "/";
}

// The file systems no archive belongs on.
static bool isPseudoFs(const std::string& fsType, const std::string& mountPoint)
{
    static const char* const skipped[] = {"proc",   "sysfs",   "devtmpfs", "tmpfs", "devpts", "squashfs",
                                           "overlay", "ramfs",  "cgroup",   "cgroup2", "efivarfs",
                                           "mqueue",  "hugetlbfs", "bpf",    "autofs", "fusectl", "tracefs",
                                           "debugfs", "configfs", "pstorefs", "securityfs", "binfmt_misc"};
    for (const char* skip : skipped) {
        if (fsType == skip)
            return true;
    }
    // The system's own roots: /boot, /efi and the like hold what they hold.
    if (mountPoint == "/boot" || mountPoint == "/boot/efi" || mountPoint == "/efi"
        || mountPoint.rfind("/boot/", 0) == 0 || mountPoint == "/proc" || mountPoint == "/sys"
        || mountPoint == "/dev" || mountPoint == "/run")
        return true;
    return false;
}

std::vector<Destination> listDestinations()
{
    std::vector<Destination> list;
    FILE* mounts = std::fopen("/proc/self/mounts", "r");
    if (!mounts)
        return list;
    char line[4096];
    while (std::fgets(line, sizeof(line), mounts)) {
        Destination destination;
        char device[1024];
        char mount[1024];
        char type[256];
        char options[2048];
        int one = 0;
        int two = 0;
        if (std::sscanf(line, "%1023s %1023s %255s %2047s %d %d", device, mount, type, options, &one, &two) < 4)
            continue;
        destination.device = device;
        destination.mountPoint = mount;
        destination.fsType = type;

        // The escapes the mount table keeps.
        std::string& point = destination.mountPoint;
        std::string unescaped;
        for (size_t at = 0; at < point.size(); ++at) {
            if (point[at] == '\\' && at + 3 < point.size()) {
                char octal[4] = {point[at + 1], point[at + 2], point[at + 3], 0};
                unescaped += char(std::strtol(octal, nullptr, 8));
                at += 3;
            } else {
                unescaped += point[at];
            }
        }
        point = unescaped;

        if (isPseudoFs(destination.fsType, point))
            continue;

        struct statvfs st;
        const bool statted = ::statvfs(point.c_str(), &st) == 0;
        if (statted) {
            // A file system may not answer the sizes, and one that answers
            // with a negative one, as some FUSE mounts do, is not a place.
            const int64_t total = int64_t(st.f_blocks) * int64_t(st.f_bsize);
            const int64_t freeSpace = int64_t(st.f_bavail) * int64_t(st.f_bsize);
            if (total > 0 && freeSpace >= 0) {
                destination.totalBytes = uint64_t(total);
                destination.freeBytes = uint64_t(freeSpace);
            }
        }
        if (destination.totalBytes == 0)
            continue;

        destination.external = point.rfind("/media/", 0) == 0 || point.rfind("/run/media/", 0) == 0
            || destination.fsType == "vfat" || destination.fsType == "exfat";
        destination.writable = (!statted || (st.f_flag & ST_RDONLY) == 0)
            && ::access(point.c_str(), W_OK) == 0;

        // A mount may sit below another; the outer disk carries the archive.
        bool covered = false;
        for (const Destination& known : list) {
            if (point.size() > known.mountPoint.size()
                && point.compare(0, known.mountPoint.size(), known.mountPoint) == 0
                && point[known.mountPoint.size()] == '/') {
                covered = true;
                break;
            }
        }
        if (!covered)
            list.push_back(std::move(destination));
    }
    std::fclose(mounts);

    // The home disk first, then the outside disks by their size.
    const std::string home = homeDirectory();
    std::sort(list.begin(), list.end(), [&home](const Destination& first, const Destination& second) {
        const bool firstHome = first.mountPoint == "/" || home.compare(0, first.mountPoint.size(), first.mountPoint) == 0;
        const bool secondHome = second.mountPoint == "/" || home.compare(0, second.mountPoint.size(), second.mountPoint) == 0;
        if (firstHome != secondHome)
            return firstHome;
        if (first.external != second.external)
            return first.external;
        return first.freeBytes > second.freeBytes;
    });
    return list;
}

std::string defaultDestinationPath()
{
    return homeDirectory() + "/Backups";
}

} // namespace cutefish::backup
