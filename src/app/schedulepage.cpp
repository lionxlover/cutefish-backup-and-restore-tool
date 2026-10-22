// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

// The schedule page: a systemd user timer that runs cutefish-backupctl with
// the same settings the wizard wrote, so a backup happens even when the
// window is never opened.

#include <ctime>
#include <functional>
#include <string>
#include <vector>

#include "app/backupwindow.h"
#include "base/i18n.h"
#include "core/scheduler.h"
#include "core/util.h"
#include "ui/combo_box.h"
#include "ui/label.h"
#include "ui/switch.h"
#include "ui/theme.h"

using namespace cutefish::ui;
using cutefish::i18n::format;
using cutefish::i18n::tr;

namespace backup {

void BackupWindow::buildSchedulePage()
{
    m_sidebar->setCurrentIndex(2);
    WizardFrame frame = makeFrame(tr("Automatic backups"),
                                   tr("A systemd timer of the user's own, with no password asked. It runs the same backup the page sets up."));

    // The timer itself: the settings app's own switch, combo and value rows,
    // against one another in one card.
    Section* timerSection = frame.content->addChild<Section>(tr("Timer"));
    auto timerCard = std::make_unique<Card>(false);
    FormRow* enableRow = timerCard->addChild<FormRow>(tr("Back up automatically"));
    Switch* enable = enableRow->setTrailing<Switch>();
    enable->setChecked(m_config.scheduleEnabled());
    enable->onToggled = [this](bool checked) { applySchedule(checked); };

    FormRow* frequencyRow = timerCard->addChild<FormRow>(tr("How often"), 52);
    ComboBox* frequency = frequencyRow->setTrailing<ComboBox>();
    frequency->setItems({tr("Daily"), tr("Weekly (Mondays)"), tr("Monthly (the first)")});
    frequency->setCurrentIndex(m_config.scheduleFrequency() == "weekly" ? 1
                               : m_config.scheduleFrequency() == "monthly" ? 2
                                                                           : 0);
    frequency->onActivated = [this, enable](int index) {
        m_config.setScheduleFrequency(index == 1   ? "weekly"
                                      : index == 2 ? "monthly"
                                                   : "daily");
        m_config.save();
        // A change while the timer is on takes effect at once.
        if (enable->isChecked())
            applySchedule(true);
    };

    FormRow* keepRow = timerCard->addChild<FormRow>(tr("Keep"), 52);
    ComboBox* keep = keepRow->setTrailing<ComboBox>();
    keep->setItems({tr("The newest 5"), tr("The newest 10"), tr("The newest 20"), tr("Every one")});
    const int count = m_config.keepCount();
    keep->setCurrentIndex(count == 5 ? 0 : count == 20 ? 2 : count == 0 ? 3 : 1);
    keep->onActivated = [this](int index) {
        m_config.setKeepCount(index == 0   ? 5
                              : index == 2 ? 20
                              : index == 3 ? 0
                                           : 10);
        m_config.save();
    };
    Label* keepAbout = timerCard->addChild<Label>(
        tr("Older archives are removed after each successful backup."));
    keepAbout->setWrap(true);
    keepAbout->setFontSize(Theme::current().smallFontSize());
    keepAbout->setColor(ColorId::DisabledText);
    LayoutHints aboutHints{.fillWidth = true};
    aboutHints.margins = {16, 0, 16, 12};
    keepAbout->setLayoutHints(aboutHints);
    timerSection->addChild(std::move(timerCard));

    // Where things stand: the value rows of the last run and the timer.
    Section* statusSection = frame.content->addChild<Section>(tr("Where things stand"));
    auto statusCard = std::make_unique<Card>(false);
    const int64_t lastTime = m_config.lastBackupTime();
    std::string lastLine;
    if (lastTime > 0 && !m_config.lastBackupName().empty()) {
        std::time_t when = time_t(lastTime);
        std::tm parts = *std::localtime(&when);
        char date[64];
        std::strftime(date, sizeof(date), "%x %H:%M", &parts);
        lastLine = format(tr("{name}, {date}"),
                          {{"name", m_config.lastBackupName()}, {"date", date}});
    } else {
        lastLine = tr("No backup has been made yet.");
    }
    FormRow* lastRow = statusCard->addChild<FormRow>(tr("The last backup"));
    Label* lastValue = lastRow->setTrailing<Label>(lastLine);
    lastValue->setColor(ColorId::DisabledText);
    lastValue->setElide(Label::Elide::Middle);
    LayoutHints lastHints = lastValue->layoutHints();
    lastHints.maximumWidth = 300;
    lastValue->setLayoutHints(lastHints);

    const bool timerOn = cutefish::backup::Scheduler::isEnabled();
    FormRow* stateRow = statusCard->addChild<FormRow>(tr("The timer"));
    Label* stateValue = stateRow->setTrailing<Label>(
        timerOn ? format(tr("On: {frequency}"),
                         {{"frequency", cutefish::backup::Scheduler::frequencyLabel(
                                            m_config.scheduleFrequency())}})
                : tr("Off"));
    stateValue->setColor(ColorId::DisabledText);
    stateValue->setElide(Label::Elide::Middle);
    LayoutHints stateHints = stateValue->layoutHints();
    stateHints.maximumWidth = 300;
    stateValue->setLayoutHints(stateHints);
    statusSection->addChild(std::move(statusCard));

    // The way to a backup this very minute.
    frame.backButton->setVisible(false);
    frameSetNext(frame, tr("Back up now"), true, [this] {
        m_sidebar->setCurrentIndex(0);
        buildBackupWhat();
    });
}

void BackupWindow::applySchedule(bool enabled)
{
    if (enabled) {
        std::string error;
        if (!cutefish::backup::Scheduler::enable(m_config.scheduleFrequency(), &error)) {
            askConfirm(tr("The timer could not be installed"), error, tr("OK"), false, nullptr, false);
            m_config.setScheduleEnabled(false);
            m_config.save();
            buildSchedulePage();
            return;
        }
        m_config.setScheduleEnabled(true);
        m_config.save();
        m_toast->show(format(tr("The timer is on: {frequency}."),
                             {{"frequency", cutefish::backup::Scheduler::frequencyLabel(
                                                m_config.scheduleFrequency())}}));
    } else {
        std::string error;
        cutefish::backup::Scheduler::disable(&error);
        m_config.setScheduleEnabled(false);
        m_config.save();
        m_toast->show(tr("The timer is off."));
    }
}

} // namespace backup
