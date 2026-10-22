// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

#include "app/backupwindow.h"

#include "base/i18n.h"
#include "ui/application.h"

int main(int argc, char** argv)
{
    cutefish::ui::Application application(argc, argv);
    application.setApplicationId("cutefish-backup");
    cutefish::i18n::setDomain("cutefish-backup");

    backup::BackupWindow window;
    window.window().show();

    return application.run();
}
