// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

// The restore wizard: which archive, which of its files, where they go, and
// the run itself. The list of files comes from the index written beside the
// archive at backup time, so browsing is instant; an archive without one
// still restores whole.

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/backupwindow.h"
#include "base/i18n.h"
#include "core/archives.h"
#include "core/util.h"
#include "ui/button.h"
#include "ui/dialog_window.h"
#include "ui/file_chooser.h"
#include "ui/label.h"
#include "ui/theme.h"

using namespace cutefish::ui;
using cutefish::i18n::format;
using cutefish::i18n::tr;

namespace backup {

static std::string dateOf(int64_t epoch)
{
    std::time_t when = time_t(epoch);
    std::tm parts = *std::localtime(&when);
    char text[64];
    std::strftime(text, sizeof(text), "%x %H:%M", &parts);
    return text;
}

void BackupWindow::buildRestorePick()
{
    m_sidebar->setCurrentIndex(1);
    WizardFrame frame = makeFrame(tr("Which backup"),
                                   tr("The archives at the destination. One is chosen here and read below."));

    auto card = std::make_unique<Card>(false);
    const std::vector<cutefish::backup::ArchiveInfo> archives =
        cutefish::backup::listArchives(m_destination);

    std::vector<IconTextRow*> rows;
    for (const cutefish::backup::ArchiveInfo& archive : archives) {
        IconTextRow* row = card->addChild<IconTextRow>("application-x-archive", 24);
        row->setTitle(archive.name);
        std::string about = format(tr("{date} · {size}"),
                                  {{"date", dateOf(archive.modified)},
                                   {"size", cutefish::backup::humanReadableSize(archive.compressedSize)}});
        if (archive.files > 0)
            about += format(tr(" · {count} files"), {{"count", std::to_string(archive.files)}});
        if (archive.meta) {
            const std::string host = archive.meta->get("hostname").toString();
            if (!host.empty())
                about += " · " + host;
        }
        row->setSubtitle(about);
        row->setCheckable(true);
        row->setChecked(archive.path == m_archivePath);
        row->onClicked = [this, rows, path = archive.path, row]() mutable {
            m_archivePath = path;
            for (IconTextRow* known : rows)
                known->setChecked(false);
            row->setChecked(true);
        };
        rows.push_back(row);
    }
    IconTextRow* otherRow = card->addChild<IconTextRow>("folder", 24);
    otherRow->setTitle(tr("Choose a file…"));
    otherRow->setSubtitle(tr("An archive anywhere on the machine"));
    otherRow->setCheckable(true);
    // A choice that is none of the listed ones is the picker's own; the
    // rows above have had their say already.
    bool listed = false;
    for (const cutefish::backup::ArchiveInfo& archive : archives)
        if (archive.path == m_archivePath)
            listed = true;
    otherRow->setChecked(!listed && !m_archivePath.empty());
    if (otherRow->isChecked())
        otherRow->setSubtitle(m_archivePath);
    otherRow->onClicked = [this, otherRow, rows] {
        FileChooser chooser(&m_window);
        chooser.setMode(FileChooser::Mode::OpenFile);
        chooser.setTitle(tr("Choose a backup archive"));
        FileChooser::Filter filter;
        filter.name = tr("Backup archives");
        filter.patterns = {"*.pb", "*.tar.xz", "*.tar.zst", "*.tar.gz", "*.tar"};
        chooser.setFilters({filter});
        chooser.onAccepted = [this, otherRow, rows](const std::vector<std::string>& paths,
                                                    const std::string&) {
            if (paths.empty())
                return;
            m_archivePath = paths.front();
            for (IconTextRow* known : rows)
                known->setChecked(false);
            otherRow->setChecked(true);
            otherRow->setSubtitle(m_archivePath);
        };
        chooser.open();
    };
    frame.content->addChild(std::move(card));

    if (archives.empty()) {
        SectionLabel* empty = frame.content->addChild<SectionLabel>(
            format(tr("No archives below {path} yet. Make one from the Back up page."),
                   {{"path", m_destination}}), true);
    }

    frame.backButton->setVisible(false);
    frameSetNext(frame, tr("Continue"), true, [this] {
        if (!m_archivePath.empty() && cutefish::backup::fileExists(m_archivePath))
            buildRestoreWhat();
    });
}

void BackupWindow::buildRestoreWhat()
{
    WizardFrame frame = makeFrame(tr("What comes back"),
                                   format(tr("From {archive}"), {{"archive", cutefish::backup::baseName(m_archivePath)}}));

    const bool hasIndex = cutefish::backup::archiveHasIndex(m_archivePath);
    if (!hasIndex)
        m_restoreFilters.clear();
    // Everything, or the picked files.
    auto choiceCard = std::make_unique<Card>(false);
    auto allRow = choiceCard->addChild<IconTextRow>("backup", 24);
    allRow->setTitle(tr("Everything"));
    allRow->setSubtitle(tr("The whole tree, as it was"));
    allRow->setCheckable(true);
    auto someRow = choiceCard->addChild<IconTextRow>("search", 24);
    someRow->setTitle(tr("Selected files"));
    someRow->setSubtitle(hasIndex ? tr("Walk the archive's tree and tick what you want")
                                  : tr("This archive has no index; everything comes back whole."));
    someRow->setCheckable(true);
    const bool pickingEverything = m_restoreFilters.empty();
    allRow->setChecked(pickingEverything);
    someRow->setChecked(!pickingEverything && hasIndex);
    allRow->onClicked = [this, allRow, someRow] {
        m_restoreFilters.clear();
        allRow->setChecked(true);
        someRow->setChecked(false);
        restoreFiltersChanged();
    };
    someRow->onClicked = [this, allRow, someRow] {
        if (!cutefish::backup::archiveHasIndex(m_archivePath))
            return;
        allRow->setChecked(false);
        someRow->setChecked(true);
        restoreFiltersChanged();
    };
    frame.content->addChild(std::move(choiceCard));

    // The archive's own tree, browsed by the level.
    if (hasIndex) {
        auto browseCard = std::make_unique<Card>(false);

        RowLayout* bar = browseCard->addChild<RowLayout>();
        bar->setSpacing(8);
        bar->setPadding({16, 6, 16, 6});
        if (!m_restoreBrowsePath.empty()) {
            Button* up = bar->addChild<Button>(tr("Up"));
            up->setLayoutHints({.verticalAlignment = Alignment::Center});
            up->onClicked = [this] {
                const size_t slash = m_restoreBrowsePath.rfind('/');
                m_restoreBrowsePath = slash == std::string::npos ? std::string()
                                                                : m_restoreBrowsePath.substr(0, slash);
                buildRestoreWhat();
            };
        }
        Label* path = bar->addChild<Label>(m_restoreBrowsePath.empty() ? tr("The whole archive")
                                                                        : m_restoreBrowsePath);
        path->setLayoutHints({.fillWidth = true, .verticalAlignment = Alignment::Center});
        path->setElide(Label::Elide::Middle);
        path->setColor(ColorRecipe(ColorId::DisabledText));
        Button* takeFolder = bar->addChild<Button>(tr("This folder"));
        takeFolder->setLayoutHints({.verticalAlignment = Alignment::Center});
        takeFolder->setEnabled(!m_restoreBrowsePath.empty());
        takeFolder->onClicked = [this] {
            if (!m_restoreBrowsePath.empty())
                toggleRestoreFilter(m_restoreBrowsePath);
        };

        std::vector<cutefish::backup::ArchiveEntry> entries;
        std::string error;
        cutefish::backup::browseIndex(m_archivePath, m_restoreBrowsePath, entries, &error);

        // The rows and their paths, so the ticks follow the filters.
        auto rowPaths = std::make_shared<std::vector<std::pair<std::string, IconTextRow*>>>();
        for (const cutefish::backup::ArchiveEntry& entry : entries) {
            IconTextRow* row = browseCard->addChild<IconTextRow>(
                entry.type == 'd' ? "folder" : "text-x-generic", 20);
            row->setTitle(entry.name);
            row->setSubtitle(cutefish::backup::humanReadableSize(entry.size));
            row->setCheckable(entry.type != 'd');
            rowPaths->push_back({entry.path, row});
            if (entry.type == 'd') {
                row->onClicked = [this, path = entry.path] {
                    m_restoreBrowsePath = path;
                    buildRestoreWhat();
                };
            } else {
                row->onClicked = [this, path = entry.path, rowPaths] {
                    toggleRestoreFilter(path);
                    for (auto& [knownPath, row] : *rowPaths)
                        row->setChecked(std::find(m_restoreFilters.begin(), m_restoreFilters.end(),
                                                  knownPath)
                                        != m_restoreFilters.end());
                    restoreFiltersChanged();
                };
            }
        }
        // The ticks as the page is built.
        for (auto& [knownPath, row] : *rowPaths)
            row->setChecked(std::find(m_restoreFilters.begin(), m_restoreFilters.end(), knownPath)
                            != m_restoreFilters.end());

        frame.content->addChild(std::move(browseCard));
    }

    frame.backButton->onClicked = [this] { buildRestorePick(); };
    frameSetNext(frame, tr("Continue"), true, [this] { buildRestoreWhere(); });
}

void BackupWindow::toggleRestoreFilter(const std::string& path)
{
    for (size_t i = 0; i < m_restoreFilters.size(); ++i) {
        if (m_restoreFilters[i] == path) {
            m_restoreFilters.erase(m_restoreFilters.begin() + long(i));
            return;
        }
    }
    m_restoreFilters.push_back(path);
}

void BackupWindow::restoreFiltersChanged()
{
    // Nothing here yet: the pages keep their own labels, and the page is
    // rebuilt wherever the ticks have to be shown.
}

void BackupWindow::buildRestoreWhere()
{
    WizardFrame frame = makeFrame(tr("Where it goes"),
                                   tr("Back where it came from, or into a folder of the user's own."));

    const bool systemArchive = m_archivePath.find("-system-") != std::string::npos;
    auto card = std::make_unique<Card>(false);

    auto originalRow = card->addChild<IconTextRow>("computer", 24);
    originalRow->setTitle(tr("The original places"));
    originalRow->setSubtitle(systemArchive ? tr("The administrator password is asked for")
                                           : tr("Over the tree as it was, /home and all"));
    originalRow->setCheckable(true);
    auto intoRow = card->addChild<IconTextRow>("user-home", 24);
    intoRow->setTitle(tr("Into a folder…"));
    intoRow->setSubtitle(m_restoreInto.empty() ? tr("The archive's whole tree below it")
                                               : m_restoreInto);
    intoRow->setCheckable(true);
    originalRow->setChecked(m_restoreOriginal);
    intoRow->setChecked(!m_restoreOriginal);
    originalRow->onClicked = [this, originalRow, intoRow] {
        m_restoreOriginal = true;
        originalRow->setChecked(true);
        intoRow->setChecked(false);
    };
    intoRow->onClicked = [this, originalRow, intoRow, systemArchive] {
        if (systemArchive) {
            m_toast->show(tr("A whole-system archive restores whole, to its own places."));
            return;
        }
        FileChooser chooser(&m_window);
        chooser.setMode(FileChooser::Mode::OpenFolder);
        chooser.setTitle(tr("Choose where the files come back"));
        chooser.onAccepted = [this, originalRow, intoRow](const std::vector<std::string>& paths,
                                                          const std::string&) {
            if (paths.empty())
                return;
            m_restoreOriginal = false;
            m_restoreInto = paths.front();
            intoRow->setChecked(true);
            originalRow->setChecked(false);
            intoRow->setSubtitle(m_restoreInto);
        };
        chooser.open();
    };
    frame.content->addChild(std::move(card));

    // The words under the card, held with it as one group.
    Section* warnSection = frame.content->addChild<Section>(std::string());
    SectionLabel* warning = warnSection->addChild<SectionLabel>(
        tr("Files with the same names are replaced. Everything else stays as it is."), true);

    frame.backButton->onClicked = [this] { buildRestoreWhat(); };
    frameSetNext(frame, tr("Restore"), true, [this] {
        askConfirm(tr("Restore now?"), tr("Files with the same names are replaced by the archive's."),
                   tr("Restore"), false, [this] { startRestore(); });
    });
}

void BackupWindow::startRestore()
{
    m_runningBackup = false;
    buildRestoreProgress();

    std::string program = cutefish::base::paths::findExecutable("cutefish-backupctl");
    if (program.empty())
        program = "/usr/bin/cutefish-backupctl";
    std::vector<std::string> arguments = {"restore", "--archive", m_archivePath, "--progress-events"};
    if (m_restoreOriginal) {
        // No --to: the CLI puts everything back below the root.
    } else {
        arguments.push_back("--to");
        arguments.push_back(m_restoreInto);
    }
    for (const std::string& filter : m_restoreFilters) {
        arguments.push_back("--path");
        arguments.push_back(filter);
    }
    // A whole-system archive's files belong to root; the CLI runs as the
    // administrator for it.
    const bool privileged = m_restoreOriginal && m_archivePath.find("-system-") != std::string::npos;

    m_runner.onEvent = [this](const JobRunner::Event& event) { jobEvent(event); };
    m_runner.onDone = [this](JobRunner::Outcome outcome, const cutefish::backup::Json& summary,
                             const std::string& errorText) { jobDone(outcome, summary, errorText); };
    m_runner.start(program, arguments, privileged);
    setJobRunning(true);
}

void BackupWindow::buildRestoreProgress()
{
    WizardFrame frame = makeFrame(tr("Restoring"), std::string());
    frame.backButton->setVisible(false);
    frameSetNext(frame, std::string(), false, nullptr);
    frame.nextButton->setVisible(false);

    auto card = std::make_unique<Card>();
    ColumnLayout* column = card->addChild<ColumnLayout>();
    column->setSpacing(12);

    RowLayout* statusRow = column->addChild<RowLayout>();
    statusRow->setSpacing(12);
    m_busy = statusRow->addChild<BusyIndicator>();
    m_busy->setLayoutHints({.verticalAlignment = Alignment::Center});
    m_progressStatus = statusRow->addChild<Label>(tr("Reading the archive…"));
    m_progressStatus->setLayoutHints({.fillWidth = true, .verticalAlignment = Alignment::Center});

    m_progress = column->addChild<ProgressBar>();
    m_progress->setLayoutHints({.fillWidth = true});
    m_progress->setVisible(false);

    m_progressCurrent = column->addChild<Label>(std::string());
    m_progressCurrent->setElide(Label::Elide::Middle);
    m_progressCurrent->setColor(ColorRecipe(ColorId::DisabledText));

    m_progressStats = column->addChild<Label>(std::string());
    m_progressStats->setColor(ColorRecipe(ColorId::DisabledText));

    frame.content->addChild(std::move(card));

    DialogButton* stop = frame.content->addChild<DialogButton>(tr("Stop the restore"), false);
    LayoutHints stopHints{.alignment = Alignment::End};
    stop->setLayoutHints(stopHints);
    stop->onClicked = [this] {
        askConfirm(tr("Stop the restore?"), tr("What is back stays; the rest is left out."), tr("Stop it"),
                   true, [this] { m_runner.cancel(); });
    };

    m_totalFiles = 0;
    m_totalBytes = 0;
}

} // namespace backup
