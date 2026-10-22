// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <optional>
#include <string>

namespace cutefish::backup {

// The automatic backup is a systemd user timer, as the desktop's own
// services are: ~/.config/systemd/user/cutefish-backup.service, started by
// cutefish-backup.timer. The timer runs the CLI, which reads the same
// settings the window wrote, so both always agree on what a backup is.
class Scheduler
{
public:
    // "daily" (twelve o'clock), "weekly" (Monday's) or "monthly" (the
    // first's); an unknown one is daily.
    static std::optional<std::string> onCalendarOf(const std::string& frequency);
    static std::string frequencyLabel(const std::string& frequency);

    // Writes the units and reloads the manager. False when systemd did not
    // answer, with `error` saying why; the units are still on disk.
    static bool enable(const std::string& frequency, std::string* error);
    // Stops the timer and takes the units away. An off timer is off, even
    // when the manager does not answer, so the return is true anyway unless
    // the files could not be removed.
    static bool disable(std::string* error);
    // Is the timer installed? False when systemd is away, as then it cannot
    // run either.
    static bool isEnabled();

    static std::string userUnitDirectory();
};

} // namespace cutefish::backup
