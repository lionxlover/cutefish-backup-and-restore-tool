// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "app/backupwindow.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <functional>

#include "base/event_loop.h"
#include "base/i18n.h"
#include "base/paths.h"
#include "core/destinations.h"
#include "core/format.h"
#include "core/scheduler.h"
#include "core/util.h"
#include "ui/button.h"
#include "ui/progress_bar.h"
#include "ui/check_box.h"
#include "ui/combo_box.h"
#include "ui/dialog_window.h"
#include "ui/file_chooser.h"
#include "ui/label.h"
#include "ui/layout.h"
#include "ui/list_view.h"
#include "ui/progress_bar.h"
#include "ui/switch.h"
#include "ui/text_field.h"
#include "ui/theme.h"
#include "ui/window_header.h"

using namespace cutefish::ui;
using namespace cutefish::backup;
using Json = cutefish::backup::Json;
using cutefish::i18n::format;
using cutefish::i18n::tr;

namespace backup {

// The settings app's page rhythm: the margin a page keeps, and the air
// under its header.
static constexpr double kPageMargin = 24;
static constexpr double kPageTopMargin = 16;
static constexpr double kSidebarWidth = 230;

BackupWindow::BackupWindow()
    : m_config(Config::defaultPath())
{
    m_config.load();
    m_customSources = m_config.sources();
    m_destination = m_config.destination();
    m_compression = m_config.compression();
    m_excludes = m_config.excludePatterns();

    buildWindow();
    buildBackupWhat();
}

BackupWindow::~BackupWindow() = default;

void BackupWindow::buildWindow()
{
    auto root = std::make_unique<Column>();
    root->setSpacing(0);

    // The header: the window's buttons at its end, the page's name from the
    // sidebar's edge, as the settings app's header holds its page's.
    m_header = root->addChild<WindowHeader>(40);
    m_headerTitle = m_header->setContent<Label>();
    m_headerTitle->setFontSize(11 * 4 / 3);
    LayoutHints titleHints{.fillWidth = true};
    titleHints.margins.left = kSidebarWidth + 9;
    titleHints.verticalAlignment = Alignment::Center;
    m_headerTitle->setLayoutHints(titleHints);

    // The body: the sidebar of pages, the pages themselves.
    auto body = std::make_unique<RowLayout>();
    body->setSpacing(0);
    m_sidebar = body->addChild<Sidebar>();
    m_sidebar->setLayoutHints({.fillHeight = true});
    m_sidebar->setTitle(tr("Backup"));
    m_sidebar->setEntries({
        {tr("Back up"), "backup", accent::blue},
        {tr("Restore"), "restore-symbolic", accent::green},
        {tr("Schedule"), "clock-app-symbolic", Color::rgb(0xFE8433)},
    });
    m_sidebar->onActivated = [this](int index) { switchMode(index); };

    m_stack = body->addChild<PageStack>();
    LayoutHints stackHints{.fillWidth = true, .fillHeight = true};
    m_stack->setLayoutHints(stackHints);
    root->addChild(std::move(body));

    m_window.setTitle(tr("Backup"));
    m_window.resize({900, 610});
    m_window.setMinimumSize({760, 520});
    m_window.setRestoresSize("backup");
    m_window.setContentView(std::move(root));

    m_toast = std::make_unique<Toast>(&m_window);
}

void BackupWindow::switchMode(int mode)
{
    // A running job is not left behind by a change of page; the user stops
    // it from its own page, where the state is plain.
    if (m_runner.isRunning()) {
        m_sidebar->setCurrentIndex(m_runningBackup ? 0 : 1);
        m_toast->show(tr("A backup is still running. Stop it from its own page."));
        return;
    }
    if (mode == 0)
        buildBackupWhat();
    else if (mode == 1)
        buildRestorePick();
    else
        buildSchedulePage();
}

void BackupWindow::setJobRunning(bool running)
{
    // The pages cannot be changed while a job works away; the controls stay
    // as they were until it is over.
    m_sidebar->setEnabled(!running);
}

BackupWindow::WizardFrame BackupWindow::makeFrame(const std::string& title, const std::string& subtitle)
{
    WizardFrame frame;
    // The page's name rides in the header, as the settings app's pages'
    // names do; the page below holds only its own work.
    m_headerTitle->setText(title);

    frame.page = new ColumnLayout();
    frame.page->setSpacing(16);
    frame.page->setPadding({kPageMargin, kPageTopMargin, kPageMargin, kPageTopMargin});

    if (!subtitle.empty())
        frame.page->addChild<SectionLabel>(subtitle, true);

    frame.scroll = frame.page->addChild<ScrollView>();
    LayoutHints scrollHints{.fillWidth = true, .fillHeight = true};
    frame.scroll->setLayoutHints(scrollHints);
    frame.content = frame.scroll->setContent<ColumnLayout>();
    // The sections' own rhythm: a section holds its label and card tight
    // together, and the page keeps the wider gap between the sections.
    frame.content->setSpacing(kPageMargin);

    // The bottom bar: the dialog's own buttons, at the end of the line.
    auto bar = std::make_unique<RowLayout>();
    bar->setSpacing(8);
    LayoutHints barHints{.alignment = Alignment::End};
    bar->setLayoutHints(barHints);
    frame.backButton = bar->addChild<DialogButton>(tr("Back"), false);
    frame.nextButton = bar->addChild<DialogButton>(tr("Continue"), true);
    frame.page->addChild(std::move(bar));

    m_stack->replace(std::unique_ptr<View>(frame.page));
    return frame;
}

void BackupWindow::frameSetNext(WizardFrame& frame, const std::string& text, bool primary,
                                std::function<void()> onNext)
{
    frame.nextButton->setText(text);
    frame.nextButton->setPrimary(primary);
    frame.nextButton->onClicked = std::move(onNext);
}

void BackupWindow::askConfirm(const std::string& title, const std::string& text,
                              const std::string& confirmText, bool destructive,
                              std::function<void()> onAccepted, bool cancelVisible)
{
    m_confirm = std::make_shared<ConfirmDialog>(&m_window, title, text);
    m_confirm->setConfirmText(confirmText.empty() ? tr("Confirm") : confirmText);
    m_confirm->setDestructive(destructive);
    m_confirm->setCancelVisible(cancelVisible);

    // The dialog lives until the event that answered it is over, freed by
    // the loop after; the file manager keeps its dialogs the same way.
    const auto dismiss = [this]() {
        std::shared_ptr<ConfirmDialog> alive = std::move(m_confirm);
        cutefish::base::EventLoop::current()->post([alive] {});
    };
    m_confirm->onAccepted = [this, dismiss, onAccepted = std::move(onAccepted)] {
        dismiss();
        if (onAccepted)
            onAccepted();
    };
    m_confirm->onRejected = [this, dismiss] { dismiss(); };
    m_confirm->show();
}

// ---------------------------------------------------------------------------
// The run: both wizards watch their job through the same slot.

void BackupWindow::jobEvent(const JobRunner::Event& event)
{
    const Json& data = event.data;
    if (event.name == "counting") {
        if (m_progressStatus) {
            m_progressStatus->setText(tr("Counting the files…"));
            m_busy->setVisible(true);
            m_progress->setVisible(false);
        }
    } else if (event.name == "estimate" || event.name == "start") {
        m_totalFiles = data.get("files").toUnsigned();
        m_totalBytes = data.get("bytes").toUnsigned();
        if (m_progressStatus && event.name == "start") {
            m_progressStatus->setText(tr("Backing the files up…"));
            m_busy->setVisible(false);
            m_progress->setVisible(true);
            m_progress->setValue(0);
        }
    } else if (event.name == "progress") {
        const uint64_t filesDone = data.get("files").toUnsigned();
        const uint64_t bytesDone = data.get("bytes").toUnsigned();
        const std::string current = data.get("current").toString();
        if (m_progress) {
            if (m_totalBytes > 0)
                m_progress->setValue(double(bytesDone) / double(m_totalBytes));
            else
                m_progress->setIndeterminate(true);
        }
        if (m_progressCurrent) {
            const size_t slash = current.rfind('/');
            m_progressCurrent->setText(slash == std::string::npos ? current : current.substr(slash + 1));
        }
        if (m_progressStats) {
            const uint64_t archiveBytes = data.get("archiveBytes").toUnsigned();
            if (m_runningBackup)
                m_progressStats->setText(format(tr("{files} files · {read} read → {archive} archive"),
                                                 {{"files", std::to_string(filesDone)},
                                                  {"read", cutefish::backup::humanReadableSize(bytesDone)},
                                                  {"archive", cutefish::backup::humanReadableSize(archiveBytes)}}));
            else
                m_progressStats->setText(format(tr("{files} files · {read} written"),
                                                {{"files", std::to_string(filesDone)},
                                                 {"read", cutefish::backup::humanReadableSize(bytesDone)}}));
        }
    } else if (event.name == "finished") {
        m_finishedArchive = data.get("archive").toString();
    }
}

void BackupWindow::jobDone(JobRunner::Outcome outcome, const Json& summary, const std::string& errorText)
{
    setJobRunning(false);
    if (outcome == JobRunner::Outcome::Cancelled) {
        if (m_runningBackup)
            buildBackupWhere();
        else
            buildRestoreWhere();
        m_toast->show(tr("The job was stopped. The unfinished archive was cleaned up."));
        return;
    }
    if (outcome == JobRunner::Outcome::Failed) {
        const std::string message = errorText.empty() ? tr("The job failed.") : errorText;
        askConfirm(tr("Something went wrong"), message, tr("OK"), false, nullptr, false);
        if (m_runningBackup)
            buildBackupWhere();
        else
            buildRestoreWhere();
        return;
    }
    if (m_runningBackup) {
        // The settings that ran are the settings worth keeping.
        m_config.setDestination(m_destination);
        m_config.setCompression(m_compression);
        m_config.setSystemMode(m_sourceChoice == SourceChoice::System);
        if (m_sourceChoice == SourceChoice::Home)
            m_config.setSources({cutefish::base::paths::home()});
        else if (m_sourceChoice == SourceChoice::Custom)
            m_config.setSources(m_customSources);
        m_config.save();
        buildBackupDone();
    } else {
        askConfirm(tr("The restore is finished"),
                   format(tr("{files} files were put back where they came from."),
                          {{"files", std::to_string(summary.get("files").toUnsigned())}}),
                   tr("OK"), false, nullptr, false);
        buildRestorePick();
    }
}

} // namespace backup
