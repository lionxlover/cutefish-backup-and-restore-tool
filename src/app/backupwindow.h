// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "app/jobrunner.h"
#include "app/widgets.h"
#include "core/archives.h"
#include "core/config.h"
#include "ui/dialog_window.h"
#include "ui/scroll_view.h"
#include "ui/toast.h"
#include "ui/window.h"

namespace cutefish::ui {
class BusyIndicator;
class ComboBox;
class DialogButton;
class Label;
class ListView;
class ProgressBar;
class Switch;
class TextField;
class WindowHeader;
} // namespace cutefish::ui

namespace backup {

// The tool's one window: the settings app's own shape — a sidebar of pages,
// the page's name in the header, cards and rows over the work, and the work
// itself done in cutefish-backupctl, started and watched from here. Back up
// and Restore each walk their pages from what to where to the run itself.
class BackupWindow
{
public:
    BackupWindow();
    ~BackupWindow();

    cutefish::ui::Window& window() { return m_window; }

private:
    // The wizard's steps, in the order the user walks them.
    enum class BackupStep {
        What,
        Where,
        Review,
        Progress,
        Done,
    };

    // What the backup wizard is set to copy.
    enum class SourceChoice {
        Home,
        Custom,
        System,
    };

    void buildWindow();
    void switchMode(int mode);
    void setJobRunning(bool running);

    // A page's frame: the content that scrolls, and Back and Next at the
    // end. The header carries the page's name; Next says and does what the
    // page itself sets.
    struct WizardFrame
    {
        cutefish::ui::ColumnLayout* page = nullptr;
        cutefish::ui::ScrollView* scroll = nullptr;
        cutefish::ui::ColumnLayout* content = nullptr;
        cutefish::ui::DialogButton* backButton = nullptr;
        cutefish::ui::DialogButton* nextButton = nullptr;
    };
    WizardFrame makeFrame(const std::string& title, const std::string& subtitle);
    // The pages' own builders; each ends with the frame's Next wired.
    void frameSetNext(WizardFrame& frame, const std::string& text, bool primary,
                      std::function<void()> onNext);

    // A question the user answers; the dialog is kept while it shows.
    void askConfirm(const std::string& title, const std::string& text, const std::string& confirmText,
                    bool destructive, std::function<void()> onAccepted, bool cancelVisible = true);

    // -- the backup wizard -------------------------------------------------
    void buildBackupWhat();
    void buildBackupWhere();
    void buildBackupReview();
    void buildBackupProgress();
    void buildBackupDone();
    void startBackup();
    void editExcludes();

    // -- the restore wizard ------------------------------------------------
    void buildRestorePick();
    void buildRestoreWhat();
    void buildRestoreWhere();
    void buildRestoreProgress();
    void startRestore();
    void browseRestorePath(const std::string& path);
    void toggleRestoreFilter(const std::string& path);
    void restoreFiltersChanged();

    // -- the schedule ------------------------------------------------------
    void buildSchedulePage();
    void applySchedule(bool enabled);

    // -- the run, both wizards share it ------------------------------------
    void jobEvent(const JobRunner::Event& event);
    void jobDone(JobRunner::Outcome outcome, const cutefish::backup::Json& summary,
                  const std::string& errorText);

    // What a run is of; the pages set these up as the user walks them.
    bool m_runningBackup = true; // progress shows this page's own words
    BackupStep m_backupStep = BackupStep::What;

    cutefish::ui::Window m_window;
    cutefish::ui::WindowHeader* m_header = nullptr;
    cutefish::ui::Label* m_headerTitle = nullptr;
    Sidebar* m_sidebar = nullptr;
    PageStack* m_stack = nullptr;
    std::unique_ptr<cutefish::ui::Toast> m_toast;

    // The settings the window keeps, saved as the user accepts them.
    cutefish::backup::Config m_config;
    SourceChoice m_sourceChoice = SourceChoice::Home;
    std::vector<std::string> m_customSources;
    std::string m_destination;
    std::string m_compression;
    std::vector<std::string> m_excludes;

    // The restore's own choices.
    std::string m_archivePath;
    std::vector<std::string> m_restoreFilters;
    std::string m_restoreBrowsePath; // where the file browser stands
    bool m_restoreOriginal = false;
    std::string m_restoreInto;

    // The pages' live parts, looked at while they are shown.
    cutefish::ui::BusyIndicator* m_busy = nullptr;
    cutefish::ui::ProgressBar* m_progress = nullptr;
    cutefish::ui::Label* m_progressStatus = nullptr;
    cutefish::ui::Label* m_progressCurrent = nullptr;
    cutefish::ui::Label* m_progressStats = nullptr;
    uint64_t m_totalFiles = 0;
    uint64_t m_totalBytes = 0;
    std::string m_finishedArchive;

    JobRunner m_runner;

    // The question being answered, kept until it is.
    std::shared_ptr<cutefish::ui::ConfirmDialog> m_confirm;
};

} // namespace backup
