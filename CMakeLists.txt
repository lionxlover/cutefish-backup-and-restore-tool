# SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.16)

project(backup
    VERSION 1.1
    DESCRIPTION "The backup tool of the Cutefish desktop"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
include(CTest)

option(BUILD_GUI "Build the window on cutefish-toolkit" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LibArchive REQUIRED IMPORTED_TARGET libarchive)

# ---------------------------------------------------------------------------
# The backup core: the archive engine, the settings, the timer's helpers. It
# leans on libarchive alone and POSIX, so the command-line tool of this tool
# builds and runs on any distribution, the Cutefish desktop or not.
add_library(backup-core STATIC
    src/core/archives.cpp
    src/core/archiveengine.cpp
    src/core/config.cpp
    src/core/destinations.cpp
    src/core/excluderules.cpp
    src/core/format.cpp
    src/core/json.cpp
    src/core/scheduler.cpp
    src/core/util.cpp
)
target_include_directories(backup-core PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(backup-core PUBLIC PkgConfig::LibArchive)

# ---------------------------------------------------------------------------
# cutefish-backupctl: the tool behind the window, and the window's own way of
# running a backup: it prints the progress as JSON events, and systemd's
# timer starts it with the same settings.
add_executable(cutefish-backupctl src/cli/main.cpp)
target_link_libraries(cutefish-backupctl PRIVATE backup-core)

install(TARGETS cutefish-backupctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/org.cutefish.backup.policy
        DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions)
# The Portable Backup's own MIME type, so the desktop knows a .pb file when
# it sees one — by its magic as much as by its name. The database itself is
# rebuilt by the packaging, with update-mime-database.
install(FILES data/mime/cutefish-backup.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)

# ---------------------------------------------------------------------------
# The window, built on the Cutefish desktop's own UI toolkit. On a machine
# without the desktop, or with BUILD_GUI off, the command-line tool and its
# engine still build and pass their tests.
if(BUILD_GUI)
    find_package(CutefishToolkit QUIET)
    if(NOT CutefishToolkit_FOUND)
        message(STATUS "cutefish-toolkit was not found: building cutefish-backupctl only. "
                       "Build https://github.com/cutefishos/cutefish-toolkit first for the window.")
    else()
        add_executable(cutefish-backup
            src/main.cpp
            src/app/backupwindow.cpp
            src/app/backupwizard.cpp
            src/app/restorewizard.cpp
            src/app/schedulepage.cpp
            src/app/jobrunner.cpp
            src/app/widgets.cpp
        )
        target_link_libraries(cutefish-backup PRIVATE backup-core Cutefish::Toolkit)

        install(TARGETS cutefish-backup RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

        # The translated desktop entry, and the icon beside the theme's own:
        # preferences-system-backup of cutefishos/icons, installed where every
        # desktop finds it whatever its theme is.
        cutefish_add_translations(cutefish-backup SOURCES src DESKTOP_FILES data/cutefish-backup.desktop)
        install(FILES ${CMAKE_CURRENT_BINARY_DIR}/cutefish-backup.desktop
                DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
        install(FILES data/icons/cutefish-backup.svg
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    endif()
endif()

# ---------------------------------------------------------------------------
# The engine's own tests, run with ctest.
if(BUILD_TESTING)
    add_executable(cutefish-backup-tests tests/test_core.cpp)
    target_link_libraries(cutefish-backup-tests PRIVATE backup-core)
    add_test(NAME core COMMAND cutefish-backup-tests)
endif()
