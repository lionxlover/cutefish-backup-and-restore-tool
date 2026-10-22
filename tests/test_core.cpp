// SPDX-FileCopyrightText: 2026 The Cutefish Backup Authors
// SPDX-License-Identifier: BSD-3-Clause

// The engine's own tests, run without any test framework: a tree is made,
// backed up into every compression, browsed, pruned and restored, and the
// files that come back are the files that went in. Run by
// scripts/build-core.sh; exit 1 on the first failure.

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <string>
#include <vector>

#include <archive.h>
#include <archive_entry.h>

#include "core/archives.h"
#include "core/archiveengine.h"
#include "core/config.h"
#include "core/excluderules.h"
#include "core/format.h"
#include "core/json.h"
#include "core/util.h"

using namespace cutefish::backup;

static std::string g_root;
static bool g_hasHardLink = false;
static bool g_hasSymlink = false;

static void makeTree()
{
    const std::string home = g_root + "/home/alice";
    makeDirectories(home + "/Documents");
    makeDirectories(home + "/Documents/Reports");
    makeDirectories(home + "/.cache/big");
    makeDirectories(home + "/Photos");
    writeFile(home + "/hello.txt", "Hello, Cutefish!\n");
    writeFile(home + "/Documents/notes.md", "# Notes\nBackups are good.\n");
    writeFile(home + "/Documents/Reports/report.txt",
              std::string(200000, 'x')); // big enough to show compression
    writeFile(home + "/Photos/picture.bin", std::string(50000, '\xAB'));
    writeFile(home + "/.cache/big/waste.log", std::string(100000, 'z')); // excluded
    writeFile(home + "/left.tmp", "temporary, excluded");
    // A hard link pair: the archive keeps the link, and the sizes show it.
    const std::string linkedFrom = home + "/Documents/notes.md";
    const std::string linkedTo = home + "/Documents/notes-copy.md";
    writeFile(linkedFrom, "# Notes\nBackups are good.\n");
    // A hard link pair: the archive keeps the link, and the sizes show it.
    // A file system may not allow making them, as some containers do not;
    // the checks below only run when the pair is there.
    g_hasHardLink = ::link(linkedFrom.c_str(), linkedTo.c_str()) == 0;
    g_hasSymlink = ::symlink("notes.md", (home + "/Documents/notes-link.md").c_str()) == 0;
    if (!g_hasHardLink || !g_hasSymlink)
        std::fprintf(stderr, "note: no hard link or symlink made (%d %d); their checks are skipped\n",
                     int(g_hasHardLink), int(g_hasSymlink));
}

static bool sameBytes(const std::string& first, const std::string& second)
{
    std::string a;
    std::string b;
    if (!readFile(first, a) || !readFile(second, b))
        return false;
    return a == b;
}

// A plain tar.xz, written straight by libarchive with no container around
// it: the file the tool's own first version made, and any other tool still
// makes. The reader must open it as it always did.
static bool writePlainTarXz(const std::string& out, const std::string& entryPath, const std::string& content)
{
    archive* a = archive_write_new();
    archive_write_set_format_pax(a);
    archive_write_add_filter_xz(a);
    if (archive_write_open_filename(a, out.c_str()) != ARCHIVE_OK) {
        archive_write_free(a);
        return false;
    }
    archive_entry* entry = archive_entry_new();
    archive_entry_copy_pathname(entry, entryPath.c_str());
    archive_entry_set_size(entry, content.size());
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_mtime(entry, std::time(nullptr), 0);
    const bool ok = archive_write_header(a, entry) == ARCHIVE_OK
        && archive_write_data(a, content.data(), content.size()) == la_ssize_t(content.size())
        && archive_write_close(a) == ARCHIVE_OK;
    archive_entry_free(entry);
    archive_write_free(a);
    return ok;
}

int main()
{
    char pattern[] = "/tmp/cutefish-backup-test-XXXXXX";
    const char* made = ::mkdtemp(pattern);
    if (!made) {
        std::fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }
    g_root = made;
    std::printf("test tree: %s\n", g_root.c_str());

    makeTree();
    const std::string home = g_root + "/home/alice";
    const std::string destination = g_root + "/backups";

    // ------------------------------------------------------------------ rules
    {
        ExcludeRules rules({".cache/", "*.tmp", "node_modules/"});
        assert(rules.matches(".cache", ".cache"));
        assert(rules.matches(".cache/big", "big"));
        assert(rules.excludesDirectory(".cache"));
        assert(!rules.excludesDirectory("Documents"));
        assert(rules.matches("Documents/draft.tmp", "draft.tmp"));
        assert(!rules.matches("Documents/notes.md", "notes.md"));
        assert(rules.matches("web/node_modules/pkg/index.js", "index.js"));
        // A named directory below a source, without its files.
        assert(rules.matches("Documents", "Documents") == false);
        std::printf("rules: ok\n");
    }

    // ------------------------------------------------------------------ json
    {
        Json value = Json::parse("{\"name\": \"cutefish\", \"files\": 1234, \"ok\": true, "
                                 "\"list\": [\"a\", \"b\"], \"none\": null}");
        assert(value.isObject());
        assert(value.get("name").toString() == "cutefish");
        assert(value.get("files").toUnsigned() == 1234);
        assert(value.get("ok").toBool());
        assert(value.get("list").size() == 2);
        assert(value.get("list").at(1).toString() == "b");
        assert(value.get("none").isNull());
        const std::string out = Json::parse("{\"a\":[1,2,{\"b\":\"c\\u00e9\"}],\"d\":{}}").dump();
        assert(!Json::parse(out).isNull());
        assert(Json::parse(out).get("a").size() == 3);
        assert(!Json::parse("{broken").isObject());
        std::printf("json: ok\n");
    }

    // ------------------------------------------------------------------ config
    {
        const std::string path = g_root + "/backup.conf";
        Config config(path);
        config.load();
        config.setSources({home});
        config.setDestination(destination);
        config.setCompression("zstd-high");
        config.setExcludePatterns({".cache/", "*.tmp"});
        config.setScheduleEnabled(true);
        config.setScheduleFrequency("weekly");
        config.setKeepCount(5);
        config.setLastBackup("archive.pb", 1729600000);
        assert(config.save());

        Config again(path);
        again.load();
        assert(again.sources().size() == 1 && again.sources().front() == home);
        assert(again.destination() == destination);
        assert(again.compression() == "zstd-high");
        assert(again.excludePatterns().size() == 2);
        assert(again.scheduleEnabled());
        assert(again.scheduleFrequency() == "weekly");
        assert(again.keepCount() == 5);
        assert(again.lastBackupName() == "archive.pb");
        assert(again.lastBackupTime() == 1729600000);
        assert(again.compressionPreset()->format == Format::Zstd);
        assert(again.compressionPreset()->level == 19);
        std::printf("config: ok\n");
    }

    // ------------------------------------------------------------------ backup
    std::string archive;
    std::string zstdArchive;
    std::string gzipArchive;
    for (Format format : {Format::Xz, Format::Zstd, Format::Gzip}) {
        BackupOptions options;
        options.sources = {home};
        options.destinationDir = destination;
        options.format = format;
        options.level = format == Format::Zstd ? 3 : 6;
        options.excludes = ExcludeRules::homeDefaults();
        options.excludes.push_back(".cache/");
        options.excludes.push_back("*.tmp");

        uint64_t estimates = 0;
        BackupHooks hooks;
        hooks.onEstimate = [&](uint64_t files, uint64_t bytes) { estimates = files + bytes; };
        uint64_t lastFiles = 0;
        hooks.onProgress = [&](uint64_t filesDone, uint64_t, uint64_t, const std::string&) {
            lastFiles = std::max(lastFiles, filesDone);
        };
        const JobResult result = runBackup(options, nullptr, hooks);
        if (result.status != JobStatus::Ok) {
            std::fprintf(stderr, "backup (%s) failed: %s\n", formatName(format).c_str(),
                         result.error.c_str());
            return 1;
        }
        assert(!result.archivePath.empty());
        assert(result.files > 0);
        assert(result.compressedBytes > 0);
        assert(estimates > 0);
        assert(lastFiles > 0);
        if (format == Format::Xz)
            archive = result.archivePath;
        if (format == Format::Zstd)
            zstdArchive = result.archivePath;
        if (format == Format::Gzip)
            gzipArchive = result.archivePath;
        std::string name = result.archivePath;
        assert(name.find("cutefish-backup-home-") != std::string::npos);
        // The one ending every backup takes, whichever codec is inside.
        assert(name.size() > 3 && name.compare(name.size() - 3, 3, ".pb") == 0);
        std::printf("backup %s: %llu files, %s into %s\n", formatName(format).c_str(),
                    (unsigned long long)result.files, humanReadableSize(result.bytes).c_str(),
                    humanReadableSize(result.compressedBytes).c_str());
    }

    // ------------------------------------------------------------------ list
    {
        const std::vector<ArchiveInfo> list = listArchives(destination);
        assert(list.size() == 3);
        assert(list.front().hasIndex);
        assert(list.front().hasMeta);
        assert(list.front().meta);
        assert(list.front().meta->get("hostname").isString());
        assert(list.front().files > 0);
        // Named by the hour the test ran, all the same one.
        for (const ArchiveInfo& info : list)
            assert(info.path.size() > 5);
        // The codec a .pb file used is read from its own header, so the
        // listing tells it though the name no longer does.
        bool sawXz = false;
        bool sawZstd = false;
        bool sawGzip = false;
        for (const ArchiveInfo& info : list) {
            if (info.format == "xz")
                sawXz = true;
            if (info.format == "zstd")
                sawZstd = true;
            if (info.format == "gzip")
                sawGzip = true;
        }
        assert(sawXz && sawZstd && sawGzip);
        std::printf("list: %zu archives, codecs read from the headers\n", list.size());
    }

    // ------------------------------------------------------------------ container
    {
        // The .pb container itself: the magic first, the codec and the level
        // in the header, the index and the details in the trailer at the
        // end — the sidecars' own bytes, carried inside.
        std::ifstream in(archive, std::ios::binary);
        char magic[4] = {0, 0, 0, 0};
        in.read(magic, 4);
        assert(in.gcount() == 4);
        assert(magic[0] == 'C' && magic[1] == 'P' && magic[2] == 'B' && (unsigned char)magic[3] == 0x01);

        pb::Header header;
        bool isPb = false;
        std::string headerError;
        assert(pb::readFileHeader(archive, header, &isPb, &headerError));
        assert(isPb);
        assert(header.codec == pb::CodecXz);
        assert((header.flags & pb::FlagTrailer) != 0);
        assert(pb::hasTrailer(archive));
        pb::Trailer trailer;
        assert(pb::readTrailer(archive, trailer));
        assert(!trailer.index.empty());
        assert(trailer.index.find("hello.txt") != std::string::npos);
        assert(!trailer.meta.empty());
        Json meta;
        assert(Json::parse(trailer.meta, meta) && meta.get("hostname").isString());
        // The payload sits between the two, and neither reaches into it.
        uint64_t start = 0;
        uint64_t end = 0;
        assert(pb::payloadRange(archive, start, end));
        struct stat st;
        assert(::stat(archive.c_str(), &st) == 0);
        assert(start == 16);
        assert(end > 16 && end < uint64_t(st.st_size)); // the trailer follows it
        std::printf("container: magic, header and trailer in place\n");
    }

    // ------------------------------------------------------------------ browse
    {
        // The archive keeps whole paths below "/": the test tree, at
        // /tmp/cutefish-backup-test-…/home/alice, sits below the same name
        // inside the archive, as /home/alice would on a real machine.
        const std::string archiveHome = home.substr(1);
        std::vector<ArchiveEntry> entries;
        std::string error;
        assert(browseIndex(archive, "", entries, &error));
        bool sawTreeRoot = false;
        for (const ArchiveEntry& entry : entries)
            if (entry.name == "tmp")
                sawTreeRoot = entry.type == 'd';
        assert(sawTreeRoot);

        assert(browseIndex(archive, archiveHome, entries, &error));
        size_t names = 0;
        bool sawCache = false;
        for (const ArchiveEntry& entry : entries) {
            if (entry.name == ".cache")
                sawCache = true;
            ++names;
        }
        assert(names == 3); // Documents, Photos, hello.txt; the caches are out
        assert(!sawCache);
        std::printf("browse: root %zu, home %zu\n", entries.size(), names);

        // The sidecars are the cache; taken away, the .pb file's own
        // trailer answers for them — a lone file carried anywhere still
        // knows its own tree.
        std::remove((archive + ".index").c_str());
        std::remove((archive + ".meta").c_str());
        assert(browseIndex(archive, archiveHome, entries, &error));
        assert(!entries.empty());
        bool sawNotesStill = false;
        for (const ArchiveEntry& entry : entries)
            if (entry.name == "Documents")
                sawNotesStill = true;
        assert(sawNotesStill);
        const std::vector<ArchiveInfo> relisted = listArchives(destination);
        assert(relisted.size() == 3);
        for (const ArchiveInfo& info : relisted) {
            if (info.path != archive)
                continue;
            assert(info.hasIndex && info.hasMeta);
            assert(info.meta);
            assert(info.meta->get("hostname").isString());
            assert(info.files > 0);
        }
        std::printf("trailer: browsed and listed with the sidecars gone\n");

        // Reading through, as an archive without any index is read.
        assert(browseArchiveByReading(archive, archiveHome + "/Documents", entries, &error));
        bool sawNotes = false;
        for (const ArchiveEntry& entry : entries)
            if (entry.name == "notes.md")
                sawNotes = true;
        assert(sawNotes);
        std::printf("browse by reading: %zu entries without an index\n", entries.size());
    }

    // ------------------------------------------------------------------ legacy archives
    {
        // A plain tar.xz, made without this tool's container, opens and
        // restores as it always did: the codec is read from the file's own
        // magic bytes, and the old endings are still listed.
        const std::string legacyDir = g_root + "/legacy";
        makeDirectories(legacyDir);
        const std::string legacy = legacyDir + "/plain.tar.xz";
        const std::string expected = g_root + "/legacy/expected.txt";
        writeFile(expected, "Hello, plain tar!\n");
        assert(writePlainTarXz(legacy, "legacy/hello.txt", "Hello, plain tar!\n"));

        const std::vector<ArchiveInfo> listed = listArchives(legacyDir);
        assert(listed.size() == 1);
        assert(listed.front().format == "xz");
        assert(listed.front().hasIndex == false); // a plain tar carries nothing

        RestoreOptions options;
        options.archivePath = legacy;
        options.destination = g_root + "/legacy-out";
        const JobResult result = runRestore(options, nullptr, RestoreHooks());
        assert(result.status == JobStatus::Ok);
        assert(sameBytes(g_root + "/legacy-out/legacy/hello.txt", expected));
        std::printf("legacy: a plain tar.xz restored as it always was\n");
    }

    // ------------------------------------------------------------------ verify
    {
        // Every codec, not one of them: a backup that only verifies under xz
        // is a backup whose other files were never truly read back.
        for (const std::string& path : {archive, zstdArchive, gzipArchive}) {
            const JobResult result = verifyArchive(path, nullptr);
            assert(result.status == JobStatus::Ok);
            assert(result.files > 0);
        }
        std::printf("verify: whole, all three codecs read end to end\n");
    }

    // ------------------------------------------------------------------ restore
    {
        RestoreOptions options;
        options.archivePath = archive;
        options.destination = g_root + "/restored";
        const JobResult result = runRestore(options, nullptr, RestoreHooks());
        assert(result.status == JobStatus::Ok);
        // The tree below the destination: the whole path the archive kept.
        const std::string back = g_root + "/restored/" + home.substr(1);
        assert(sameBytes(back + "/hello.txt", home + "/hello.txt"));
        assert(sameBytes(back + "/Documents/notes.md", home + "/Documents/notes.md"));
        assert(sameBytes(back + "/Documents/Reports/report.txt", home + "/Documents/Reports/report.txt"));
        assert(sameBytes(back + "/Photos/picture.bin", home + "/Photos/picture.bin"));
        // The excluded files never came back.
        assert(!fileExists(back + "/.cache/big/waste.log"));
        assert(!fileExists(back + "/left.tmp"));
        // A symlink restored as itself, and a hard link pair restored as
        // one inode, when the file system let the tree have them.
        struct stat st;
        if (g_hasSymlink)
            assert(::lstat((back + "/Documents/notes-link.md").c_str(), &st) == 0 && S_ISLNK(st.st_mode));
        if (g_hasHardLink) {
            struct stat first;
            struct stat second;
            assert(::stat((back + "/Documents/notes.md").c_str(), &first) == 0);
            assert(::stat((back + "/Documents/notes-copy.md").c_str(), &second) == 0);
            assert(first.st_ino == second.st_ino);
        }
        std::printf("restore: %llu files, whole tree back\n", (unsigned long long)result.files);
    }

    // ------------------------------------------------------------------ every codec restores
    {
        // The same bytes out of zstd and gzip: the codec is the file's own
        // business, written in its header, and every one restores whole.
        for (const std::string& path : {zstdArchive, gzipArchive}) {
            RestoreOptions options;
            options.archivePath = path;
            options.destination = g_root + "/restored-" + path.substr(path.rfind('/') + 1);
            const JobResult result = runRestore(options, nullptr, RestoreHooks());
            assert(result.status == JobStatus::Ok);
            const std::string back = options.destination + "/" + home.substr(1);
            assert(sameBytes(back + "/hello.txt", home + "/hello.txt"));
            assert(sameBytes(back + "/Documents/Reports/report.txt",
                             home + "/Documents/Reports/report.txt"));
        }
        std::printf("restore: zstd and gzip whole, byte for byte\n");
    }

    // ------------------------------------------------------------------ partial restore
    {
        RestoreOptions options;
        options.archivePath = archive;
        options.destination = g_root + "/partial";
        options.paths = {home.substr(1) + "/Documents/Reports"};
        const JobResult result = runRestore(options, nullptr, RestoreHooks());
        assert(result.status == JobStatus::Ok);
        assert(fileExists(g_root + "/partial/" + home.substr(1) + "/Documents/Reports/report.txt"));
        assert(!fileExists(g_root + "/partial/" + home.substr(1) + "/hello.txt"));
        std::printf("partial restore: only the reports came back\n");
    }

    // ------------------------------------------------------------------ destination not swallowed
    {
        // A destination below a source — ~/Backups inside ~ — holds its own
        // archives; the walk must step over it, or every run copies the last
        // archive into the next.
        const std::string inside = g_root + "/inside";
        const std::string insideBackups = inside + "/Backups";
        makeDirectories(inside);
        writeFile(inside + "/letter.txt", "to the point");
        makeDirectories(insideBackups);

        BackupOptions options;
        options.sources = {inside};
        options.destinationDir = insideBackups;
        options.skipBelow = insideBackups;
        options.format = Format::Xz;
        options.level = 3;
        JobResult first = runBackup(options, nullptr, BackupHooks());
        assert(first.status == JobStatus::Ok);
        JobResult second = runBackup(options, nullptr, BackupHooks());
        assert(second.status == JobStatus::Ok);
        assert(second.files == first.files); // the first archive changed nothing

        std::vector<ArchiveEntry> entries;
        std::string error;
        assert(browseIndex(second.archivePath, inside.substr(1), entries, &error));
        for (const ArchiveEntry& entry : entries)
            assert(entry.name != "Backups");
        std::printf("destination skipped: a backup beside itself stays the same size\n");
    }

    // ------------------------------------------------------------------ prune
    {
        const std::vector<std::string> removed = pruneOldArchives(destination, 2);
        assert(removed.size() == 1); // three made, two kept
        assert(listArchives(destination).size() == 2);
        std::printf("prune: removed %zu archive(s)\n", removed.size());
    }

    std::printf("all engine tests passed\n");
    return 0;
}
