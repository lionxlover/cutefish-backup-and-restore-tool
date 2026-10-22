// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cutefish::backup {

// Where a backup can go. The window's list of places is the mounts the
// kernel shows, without the ones that are no place for an archive: the
// system's own interfaces, read-only disks, and the swap.
struct Destination
{
    std::string mountPoint;
    std::string device;  // "/dev/sdb1", "tmpfs" and the like
    std::string fsType;  // "ext4", "vfat" …
    uint64_t totalBytes = 0;
    uint64_t freeBytes = 0;
    // A disk on a wire: mounted below /media or /run/media, or a vfat or
    // exfat disk, which almost always is one.
    bool external = false;
    // The user may make a directory below the mount.
    bool writable = false;
};

// Every mount an archive may sit on, the home folder's own disk included.
std::vector<Destination> listDestinations();

// The default folder the tool suggests: ~/Backups, below the home mount.
std::string defaultDestinationPath();

} // namespace cutefish::backup
