// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/process.h"
#include "core/json.h"

namespace cutefish::ui {
class Window;
}

namespace backup {

// The core's Json, spelled out: the window is a backup:: of its own, not a
// cutefish::backup, so the two namespaces are named apart.
using CoreJson = cutefish::backup::Json;

// Runs cutefish-backupctl for the window: a backup, a restore or a verify,
// with the progress it prints as one JSON object a line. The window never
// blocks; the process works away and reports in here, and Cancel tells it to
// stop by the same signal a terminal would.
class JobRunner
{
public:
    // How a job ended. The exit codes are the CLI's own: 0 well, 1 failed,
    // 2 cancelled.
    enum class Outcome {
        Finished,
        Failed,
        Cancelled,
    };

    // One event the CLI printed. `data` holds the event's own fields.
    struct Event
    {
        std::string name;
        CoreJson data;
    };

    JobRunner();
    ~JobRunner();

    // Starts the CLI with `arguments` behind the command, reading the
    // events it prints on stdout. `privileged` runs it through pkexec, as
    // the whole-system backup needs the administrator's blessing; the
    // program's full path must be given then, as pkexec does not look on
    // the PATH the way a shell does.
    void start(const std::string& program, const std::vector<std::string>& arguments, bool privileged);
    void cancel();
    bool isRunning() const { return m_process && m_process->isRunning(); }

    std::function<void(const Event&)> onEvent;

    // The job is over: how, with the last whole event of the run, and the
    // reason it failed when it did. `summary` is "finished" with the
    // archive in it, or an error's message.
    std::function<void(Outcome outcome, const CoreJson& summary, const std::string& errorText)> onDone;

private:
    void finished(int exitCode, bool crashed);

    std::unique_ptr<cutefish::base::Process> m_process;
    std::string m_pending; // the tail of a line not yet whole
    CoreJson m_summary;   // the last "finished" or "error" event
    std::string m_errorText;
};

} // namespace backup
