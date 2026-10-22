// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "scheduler.h"

#include <cstdlib>
#include <fstream>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
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

// Runs a program and waits for it; the return is its exit code, or -1 when
// it could not be started. Its output is not looked at.
static int run(const char* const* arguments)
{
    pid_t child = ::fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        ::execvp(arguments[0], const_cast<char* const*>(arguments));
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

std::optional<std::string> Scheduler::onCalendarOf(const std::string& frequency)
{
    if (frequency == "weekly")
        return std::string("Mon *-*-* 12:00:00");
    if (frequency == "monthly")
        return std::string("*-*-01 12:00:00");
    if (frequency == "daily")
        return std::string("*-*-* 12:00:00");
    return std::nullopt;
}

std::string Scheduler::frequencyLabel(const std::string& frequency)
{
    if (frequency == "weekly")
        return "Weekly (Mondays)";
    if (frequency == "monthly")
        return "Monthly (the first)";
    return "Daily";
}

std::string Scheduler::userUnitDirectory()
{
    return homeDirectory() + "/.config/systemd/user";
}

bool Scheduler::enable(const std::string& frequency, std::string* error)
{
    const std::optional<std::string> calendar = onCalendarOf(frequency);
    if (!calendar) {
        if (error)
            *error = "Unknown frequency: " + frequency;
        return false;
    }

    const std::string directory = userUnitDirectory();
    if (!makeDirectories(directory)) {
        if (error)
            *error = "The user services folder could not be created: " + directory;
        return false;
    }

    // The service runs the CLI, which follows the settings the window
    // wrote; the timer's own clock is the only thing it holds.
    {
        std::ofstream service(directory + "/cutefish-backup.service");
        service << "[Unit]\n"
                << "Description=Cutefish Backup\n"
                << "Documentation=https://github.com/cutefishos/cutefish-backup\n"
                << "\n"
                << "[Service]\n"
                << "Type=oneshot\n"
                << "ExecStart=/usr/bin/cutefish-backupctl backup --from-config\n";
    }
    {
        std::ofstream timer(directory + "/cutefish-backup.timer");
        timer << "[Unit]\n"
              << "Description=Run Cutefish Backup regularly\n"
              << "\n"
              << "[Timer]\n"
              << "OnCalendar=" << *calendar << "\n"
              << "Persistent=true\n"
              << "\n"
              << "[Install]\n"
              << "WantedBy=timers.target\n";
    }

    const char* reload[] = {"systemctl", "--user", "daemon-reload", nullptr};
    const char* start[] = {"systemctl", "--user", "enable", "--now", "cutefish-backup.timer", nullptr};
    if (run(reload) != 0 || run(start) != 0) {
        if (error)
            *error = "systemd did not take the timer; is systemd running in this session?";
        return false;
    }
    return true;
}

bool Scheduler::disable(std::string* error)
{
    const char* stop[] = {"systemctl", "--user", "disable", "--now", "cutefish-backup.timer", nullptr};
    run(stop);
    const char* reload[] = {"systemctl", "--user", "daemon-reload", nullptr};
    run(reload);
    const std::string directory = userUnitDirectory();
    bool off = true;
    if (!removeFile(directory + "/cutefish-backup.timer"))
        off = false;
    if (!removeFile(directory + "/cutefish-backup.service"))
        off = false;
    if (!off && error)
        *error = "The timer's units could not be removed.";
    return off;
}

bool Scheduler::isEnabled()
{
    const char* check[] = {"systemctl", "--user", "is-enabled", "--quiet", "cutefish-backup.timer", nullptr};
    return run(check) == 0;
}

} // namespace cutefish::backup
