// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

// The back-up wizard: what to copy, where to keep it, the plan and the run.

#include <chrono>
#include <functional>
#include <string>
#include <vector>

#include "app/backupwindow.h"
#include "base/i18n.h"
#include "base/paths.h"
#include "core/archives.h"
#include "core/destinations.h"
#include "core/format.h"
#include "core/util.h"
#include "ui/button.h"
#include "ui/check_box.h"
#include "ui/combo_box.h"
#include "ui/dialog_window.h"
#include "ui/file_chooser.h"
#include "ui/icon.h"
#include "ui/label.h"
#include "ui/list_view.h"
#include "ui/progress_bar.h"
#include "ui/text_field.h"
#include "ui/theme.h"

using namespace cutefish::ui;
using cutefish::i18n::format;
using cutefish::i18n::tr;

namespace backup {

void BackupWindow::buildBackupWhat()
{
    m_backupStep = BackupStep::What;
    m_sidebar->setCurrentIndex(0);
    WizardFrame frame = makeFrame(tr("What to back up"),
                                   tr("The folders whose loss would hurt. Caches and the trash are left out on their own."));

    // The choice: the whole home folder, folders of the user's own, or the
    // whole system.
    auto choiceCard = std::make_unique<Card>(false);
    auto homeRow = choiceCard->addChild<IconTextRow>("user-home", 24);
    homeRow->setTitle(tr("My home folder"));
    homeRow->setSubtitle(cutefish::base::paths::home());
    homeRow->setCheckable(true);
    auto customRow = choiceCard->addChild<IconTextRow>("folder", 24);
    customRow->setTitle(tr("Folders I choose"));
    customRow->setSubtitle(m_sourceChoice == SourceChoice::Custom && !m_customSources.empty()
                               ? format(tr("{count} folders"), {{"count", std::to_string(m_customSources.size())}})
                               : tr("Pick folders one by one"));
    customRow->setCheckable(true);
    auto systemRow = choiceCard->addChild<IconTextRow>("computer", 24);
    systemRow->setTitle(tr("Everything (the whole system)"));
    systemRow->setSubtitle(tr("The administrator password is asked for"));
    systemRow->setCheckable(true);
    IconTextRow* customRowPointer = customRow;
    IconTextRow* homeRowPointer = homeRow;
    IconTextRow* systemRowPointer = systemRow;

    const auto pick = [=, this](SourceChoice choice) {
        m_sourceChoice = choice;
        homeRowPointer->setChecked(choice == SourceChoice::Home);
        customRowPointer->setChecked(choice == SourceChoice::Custom);
        systemRowPointer->setChecked(choice == SourceChoice::System);
    };
    homeRow->onClicked = [this, pick] { pick(SourceChoice::Home); };
    customRow->onClicked = [this, pick, customRowPointer] {
        pick(SourceChoice::Custom);
        // The folders are picked in the file dialog, one at a time.
        FileChooser chooser(&m_window);
        chooser.setMode(FileChooser::Mode::OpenFolder);
        chooser.setTitle(tr("Choose a folder to back up"));
        chooser.onAccepted = [this, customRowPointer](const std::vector<std::string>& paths,
                                                      const std::string&) {
            if (paths.empty())
                return;
            std::string folder = paths.front();
            for (const std::string& known : m_customSources)
                if (known == folder)
                    return;
            m_customSources.push_back(folder);
            customRowPointer->setSubtitle(format(tr("{count} folders"),
                                                 {{"count", std::to_string(m_customSources.size())}}));
        };
        chooser.open();
    };
    systemRow->onClicked = [this, pick] { pick(SourceChoice::System); };
    pick(m_sourceChoice);

    // The custom folders picked so far, each with its own way out.
    for (const std::string& source : m_customSources) {
        IconTextRow* row = choiceCard->addChild<IconTextRow>("folder", 16);
        row->setTitle(source);
        row->setCheckable(true);
        row->setChecked(true);
        row->onClicked = [this, row] {
            // A ticked custom folder is taken out of the list.
            const std::string path = row->title();
            for (size_t i = 0; i < m_customSources.size(); ++i) {
                if (m_customSources[i] == path) {
                    m_customSources.erase(m_customSources.begin() + long(i));
                    break;
                }
            }
            row->setVisible(false);
        };
    }

    Section* choiceSection = frame.content->addChild<Section>(tr("What"));
    choiceSection->addChild(std::move(choiceCard));

    // The exclude patterns, kept in their own card with the editor's door.
    Section* excludeSection = frame.content->addChild<Section>(tr("Leave out"));
    auto excludeCard = std::make_unique<Card>(false);
    IconTextRow* excludeRow = excludeCard->addChild<IconTextRow>("edit-delete", 24);
    excludeRow->setTitle(tr("Exclude patterns"));
    excludeRow->setSubtitle(m_excludes.empty()
                                ? tr("The built-in ones only: caches, the trash, *.tmp")
                                : format(tr("{count} of the user's own, on top of the built-in ones"),
                                         {{"count", std::to_string(m_excludes.size())}}));
    excludeRow->onClicked = [this] { editExcludes(); };
    excludeSection->addChild(std::move(excludeCard));

    frame.backButton->setVisible(false);
    frameSetNext(frame, tr("Continue"), true, [this] { buildBackupWhere(); });
}

void BackupWindow::editExcludes()
{
    DialogWindow dialog(&m_window, tr("Exclude patterns"), 420);
    ColumnLayout* page = dialog.setContent<ColumnLayout>();
    page->setSpacing(12);
    Label* about = page->addChild<Label>(
        tr("A pattern keeps files out of every backup:\n\n"
           "  *.o            every file whose name ends in .o\n"
           "  Documents/     a folder by that name, anywhere below a source, and everything in it\n"
           "  /home/user/X   a folder by its path below a source\n"));
    about->setWrap(true);

    auto listCard = std::make_unique<Card>(false);
    ListView* list = listCard->addChild<ListView>();
    list->setRowHeight(40);
    list->createItem = [] {
        auto row = std::make_unique<RowLayout>();
        row->setPadding({16, 0, 8, 0});
        row->setSpacing(8);
        Label* pattern = row->addChild<Label>(std::string());
        pattern->setLayoutHints({.fillWidth = true, .verticalAlignment = Alignment::Center});
        pattern->setElide(Label::Elide::Middle);
        DialogButton* remove = row->addChild<DialogButton>(tr("Remove"), false);
        remove->setLayoutHints({.verticalAlignment = Alignment::Center});
        return row;
    };
    ListView* listPointer = list;
    list->bindItem = [this, listPointer](View* item, size_t row) {
        RowLayout* line = static_cast<RowLayout*>(item);
        if (row >= m_excludes.size())
            return;
        const std::string pattern = m_excludes[row];
        static_cast<Label*>(line->children().front().get())->setText(pattern);
        DialogButton* remove = static_cast<DialogButton*>(line->children().at(1).get());
        remove->onClicked = [this, listPointer, row, pattern] {
            if (row >= m_excludes.size())
                return;
            m_excludes.erase(m_excludes.begin() + long(row));
            listPointer->setRowCount(m_excludes.size());
            listPointer->reload();
            m_config.setExcludePatterns(m_excludes);
            m_config.save();
        };
    };
    list->setRowCount(m_excludes.size());
    page->addChild(std::move(listCard));

    // A pattern goes in with the field's own Return.
    auto addRow = std::make_unique<RowLayout>();
    addRow->setSpacing(8);
    TextField* field = addRow->addChild<TextField>();
    field->setPlaceholder(tr("A pattern, such as *.iso or Videos/"));
    field->setLayoutHints({.fillWidth = true, .verticalAlignment = Alignment::Center});
    Button* add = addRow->addChild<Button>(tr("Add"));
    auto addPattern = [this, field, listPointer] {
        std::string pattern = field->text();
        while (!pattern.empty() && pattern.back() == ' ')
            pattern.pop_back();
        if (pattern.empty())
            return;
        for (const std::string& known : m_excludes)
            if (known == pattern)
                return;
        m_excludes.push_back(pattern);
        field->clear();
        listPointer->setRowCount(m_excludes.size());
        listPointer->reload();
        m_config.setExcludePatterns(m_excludes);
        m_config.save();
    };
    add->onClicked = addPattern;
    field->onAccepted = addPattern;
    page->addChild(std::move(addRow));

    // The page behind shows the new count once the editor is out of the way.
    dialog.onClosed = [this] {
        if (!m_runner.isRunning() && m_backupStep == BackupStep::What)
            buildBackupWhat();
    };
    dialog.show();
}

void BackupWindow::buildBackupWhere()
{
    m_backupStep = BackupStep::Where;
    WizardFrame frame = makeFrame(tr("Where to keep it"),
                                   tr("The archive lands in one piece, heavily compressed. Outside disks are found on their own."));

    // The places the kernel shows, the home folder's own disk first.
    const std::vector<cutefish::backup::Destination> targets = cutefish::backup::listDestinations();
    Section* whereSection = frame.content->addChild<Section>(tr("Destination"));
    auto whereCard = std::make_unique<Card>(false);

    const std::string home = cutefish::base::paths::home();
    const std::string backupsFolder = cutefish::backup::defaultDestinationPath();

    // A place and its row, kept together so a choice can light the rows
    // again wherever it came from.
    struct Place
    {
        std::string path; // empty: the folder picker
        std::string icon;
        std::string title;
        std::string subtitle;
        IconTextRow* row = nullptr;
    };
    std::vector<Place> places;
    places.push_back({backupsFolder, "user-home", tr("A Backups folder in Home"),
                      format(tr("Inside {path}"), {{"path", backupsFolder}})});
    for (const auto& target : targets) {
        if (target.mountPoint == "/" || !target.writable)
            continue;
        if (home.compare(0, target.mountPoint.size(), target.mountPoint) == 0)
            continue; // the home disk, already offered above
        places.push_back({target.mountPoint, target.external ? "drive-removable-media" : "drive-harddisk",
                          target.mountPoint,
                          format(tr("{free} free · {type}"),
                                 {{"free", cutefish::backup::humanReadableSize(target.freeBytes)},
                                  {"type", target.fsType}})});
    }
    places.push_back({std::string(), "folder", tr("Choose a folder…"),
                      tr("A folder anywhere, or a place the list does not show")});

    for (Place& place : places) {
        place.row = whereCard->addChild<IconTextRow>(place.icon, 24);
        place.row->setTitle(place.title);
        place.row->setSubtitle(place.subtitle);
        place.row->setCheckable(true);
    }

    // The picker's row is looked at again below, outside the loop.
    const std::vector<Place> stable = places;

    const auto mark = [this, stable]() {
        bool known = false;
        for (const Place& place : stable) {
            if (!place.path.empty() && place.path == m_destination) {
                known = true;
                break;
            }
        }
        for (const Place& place : stable) {
            if (!place.path.empty())
                place.row->setChecked(place.path == m_destination);
        }
        // The picker's row shows a choice of its own.
        IconTextRow* picker = stable.back().row;
        if (!known && !m_destination.empty()) {
            picker->setChecked(true);
            picker->setSubtitle(m_destination);
        } else {
            picker->setChecked(false);
            picker->setSubtitle(stable.back().subtitle);
        }
    };

    for (Place& place : places) {
        if (!place.path.empty()) {
            std::string path = place.path;
            place.row->onClicked = [this, path, mark] {
                m_destination = path;
                mark();
            };
        } else {
            place.row->onClicked = [this, mark, picker = place.row] {
                FileChooser chooser(&m_window);
                chooser.setMode(FileChooser::Mode::OpenFolder);
                chooser.setTitle(tr("Choose where the archives go"));
                chooser.onAccepted = [this, mark, picker](const std::vector<std::string>& paths,
                                                          const std::string&) {
                    if (paths.empty())
                        return;
                    m_destination = paths.front();
                    picker->setSubtitle(m_destination);
                    mark();
                };
                chooser.open();
            };
        }
    }
    mark();
    whereSection->addChild(std::move(whereCard));

    // The compression, as the presets spell it: a row with the preset at
    // its end, and the words about it under the row, both inside the card.
    Section* compressionSection = frame.content->addChild<Section>(tr("Compression"));
    auto compressionCard = std::make_unique<Card>(false);
    FormRow* presetRow = compressionCard->addChild<FormRow>(tr("Preset"), 52);
    ComboBox* combo = presetRow->setTrailing<ComboBox>();
    const std::vector<cutefish::backup::CompressionPreset>& presets =
        cutefish::backup::compressionPresets();
    std::vector<std::string> names;
    for (const auto& preset : presets)
        names.push_back(preset.label);
    combo->setItems(names);
    int current = 0;
    for (size_t i = 0; i < presets.size(); ++i)
        if (presets[i].id == m_compression)
            current = int(i);
    combo->setCurrentIndex(current);
    combo->onActivated = [this, presets](int index) {
        if (index >= 0 && index < int(presets.size()))
            m_compression = presets[size_t(index)].id;
    };
    Label* about = compressionCard->addChild<Label>(
        tr("xz is LZMA2, the codec 7z uses: the smallest archives, the slowest to make. "
           "zstd runs rings around it and stays small; gzip reads on any machine."));
    about->setWrap(true);
    about->setFontSize(Theme::current().smallFontSize());
    about->setColor(ColorId::DisabledText);
    LayoutHints aboutHints{.fillWidth = true};
    aboutHints.margins = {16, 0, 16, 12};
    about->setLayoutHints(aboutHints);
    compressionSection->addChild(std::move(compressionCard));

    frame.backButton->onClicked = [this] { buildBackupWhat(); };
    frameSetNext(frame, tr("Review"), true, [this] { buildBackupReview(); });
}

void BackupWindow::buildBackupReview()
{
    m_backupStep = BackupStep::Review;
    WizardFrame frame = makeFrame(tr("The plan"), std::string());

    // What was set, in the settings app's own label-and-value rows.
    auto planCard = std::make_unique<Card>(false);
    const std::string what = m_sourceChoice == SourceChoice::Home
        ? tr("The home folder")
        : m_sourceChoice == SourceChoice::System
            ? tr("Everything (the whole system)")
            : format(tr("{count} folders"), {{"count", std::to_string(m_customSources.size())}});
    const std::string compression = m_compression == "zstd-fast" ? tr("zstd — fastest")
                                    : m_compression == "zstd-high" ? tr("zstd — high")
                                    : m_compression == "gzip"      ? tr("gzip — compatible")
                                                                   : tr("xz (LZMA2) — smallest");
    const std::vector<std::pair<std::string, std::string>> plan = {
        {tr("What"), what},
        {tr("Where"), m_destination},
        {tr("Compression"), compression},
        {tr("Excludes"), m_excludes.empty() ? tr("The built-in ones")
                                            : format(tr("{count} of the user's own"),
                                                     {{"count", std::to_string(m_excludes.size())}})},
    };
    for (const auto& [name, value] : plan) {
        FormRow* row = planCard->addChild<FormRow>(name);
        Label* valueLabel = row->setTrailing<Label>(value);
        valueLabel->setColor(ColorId::DisabledText);
        valueLabel->setElide(Label::Elide::Middle);
        LayoutHints valueHints = valueLabel->layoutHints();
        valueHints.maximumWidth = 320;
        valueLabel->setLayoutHints(valueHints);
    }

    // The card, and the words about it, held together as one group.
    Section* planSection = frame.content->addChild<Section>(std::string());
    planSection->addChild(std::move(planCard));
    planSection->addChild<SectionLabel>(
        tr("The backup is one .pb file — the compression above inside it, and its "
           "own file list at the end — so a single file carries everything. The "
           "progress page tells the size as it counts."), true);

    frame.backButton->onClicked = [this] { buildBackupWhere(); };
    frameSetNext(frame, tr("Start the backup"), true, [this] { startBackup(); });
}

void BackupWindow::startBackup()
{
    m_runningBackup = true;
    buildBackupProgress();

    // The settings are written before the run, so the timer and the shell
    // find the same ones.
    m_config.setDestination(m_destination);
    m_config.setCompression(m_compression);
    m_config.setSystemMode(m_sourceChoice == SourceChoice::System);
    m_config.setExcludePatterns(m_excludes);
    if (m_sourceChoice == SourceChoice::Home)
        m_config.setSources({cutefish::base::paths::home()});
    else if (m_sourceChoice == SourceChoice::Custom)
        m_config.setSources(m_customSources);
    m_config.save();

    std::string program = cutefish::base::paths::findExecutable("cutefish-backupctl");
    if (program.empty())
        program = "/usr/bin/cutefish-backupctl";
    std::vector<std::string> arguments;
    bool privileged = false;
    if (m_sourceChoice == SourceChoice::System) {
        // pkexec leaves the home folder behind; everything is spelled out.
        arguments = {"backup", "--system", "--source", "/", "--destination", m_destination,
                     "--compression", m_compression, "--progress-events"};
        privileged = true;
    } else {
        arguments = {"backup", "--from-config", "--progress-events"};
    }

    m_runner.onEvent = [this](const JobRunner::Event& event) { jobEvent(event); };
    m_runner.onDone = [this](JobRunner::Outcome outcome, const cutefish::backup::Json& summary,
                             const std::string& errorText) { jobDone(outcome, summary, errorText); };
    m_runner.start(program, arguments, privileged);
    setJobRunning(true);
}

void BackupWindow::buildBackupProgress()
{
    m_backupStep = BackupStep::Progress;
    WizardFrame frame = makeFrame(tr("Backing up"), std::string());
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
    m_progressStatus = statusRow->addChild<Label>(tr("Counting the files…"));
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

    DialogButton* stop = frame.content->addChild<DialogButton>(tr("Stop the backup"), false);
    LayoutHints stopHints{.alignment = Alignment::End};
    stop->setLayoutHints(stopHints);
    stop->onClicked = [this] {
        askConfirm(tr("Stop the backup?"), tr("The half-written archive is cleaned up."), tr("Stop it"),
                   true, [this] { m_runner.cancel(); });
    };

    m_totalFiles = 0;
    m_totalBytes = 0;
}

void BackupWindow::buildBackupDone()
{
    m_backupStep = BackupStep::Done;
    WizardFrame frame = makeFrame(tr("Finished"), std::string());
    frame.backButton->setVisible(false);

    auto card = std::make_unique<Card>();
    ColumnLayout* column = card->addChild<ColumnLayout>();
    column->setSpacing(8);
    RowLayout* iconRow = column->addChild<RowLayout>();
    iconRow->setSpacing(12);
    Icon* check = iconRow->addChild<Icon>("check", 22);
    check->setColor(accent::green);
    Label* done = iconRow->addChild<Label>(tr("Everything was copied."));
    done->setWeight(600);
    if (!m_finishedArchive.empty()) {
        Label* archive = column->addChild<Label>(m_finishedArchive);
        archive->setElide(Label::Elide::Middle);
        archive->setColor(ColorId::DisabledText);
    }
    frame.content->addChild(std::move(card));

    SectionLabel* hint = frame.content->addChild<SectionLabel>(
        tr("The archive sits beside its index, which the restore page reads to "
           "browse the files. Keep copies of the archive on another disk."), true);

    frameSetNext(frame, tr("Back up again"), true, [this] { buildBackupWhat(); });
    // The frame's Next is the way back to the start; the window's close
    // button ends the tool, as a utility's does.
}

} // namespace backup
