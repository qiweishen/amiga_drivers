// SPDX-License-Identifier: BSD-3-Clause
// The .sbf file is the record of truth: a write that cannot land must be
// visible to the caller, never a log line the rig keeps recording through.
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

#include <QByteArray>
#include <doctest/doctest.h>

#include "sbf_writer.h"

namespace {
    std::filesystem::path Scratch(const std::string &name) {
        const auto p = std::filesystem::temp_directory_path() /
                       ("asterx_writer_" + std::to_string(::getpid()) + "_" + name);
        std::filesystem::remove_all(p);
        return p;
    }

    QByteArray Block(int size, char fill = 'x') {
        return QByteArray(size, fill);
    }

    std::size_t CountSbfFiles(const std::filesystem::path &dir) {
        std::size_t n = 0;
        for (const auto &e: std::filesystem::directory_iterator(dir)) {
            if (e.path().extension() == ".sbf") {
                ++n;
            }
        }
        return n;
    }
} // namespace

TEST_CASE("SbfWriter: ConstructionFailsWhenTheOutputDirectoryCannotExist") {
    const auto blocker = Scratch("blocker");
    { std::ofstream f(blocker); f << "not a directory"; }

    asterx::SbfWriter::Config cfg;
    cfg.output_dir = blocker / "recordings"; // a path below a regular file
    CHECK_THROWS_AS(asterx::SbfWriter{cfg}, std::runtime_error);

    std::filesystem::remove_all(blocker);
}

TEST_CASE("SbfWriter: WritesRotatesAndNamesSequentially") {
    const auto dir = Scratch("rotate");
    asterx::SbfWriter::Config cfg;
    cfg.output_dir = dir;
    cfg.file_prefix = "asterx";
    cfg.rotate_bytes = 100;
    cfg.rotate_interval = std::chrono::hours(1);

    asterx::SbfWriter w(cfg);
    CHECK(std::filesystem::is_directory(dir));
    CHECK(w.Stats().records_written == 0u);

    // 64 bytes fits; the second 64 would exceed the 100-byte cap, so the
    // pre-check opens the next file instead of overshooting it.
    CHECK(w.WriteBlock(Block(64)));
    CHECK(w.WriteBlock(Block(64)));
    CHECK(w.WriteBlock(Block(64)));
    w.close();

    CHECK(w.Stats().records_written == 3u);
    CHECK(w.Stats().bytes_written == 192u);
    CHECK(w.Stats().files_opened == 3u);
    CHECK(CountSbfFiles(dir) == 3u);
    for (const auto &e: std::filesystem::directory_iterator(dir)) {
        CHECK(e.path().extension() == ".sbf");
        CHECK(e.path().filename().string().rfind("asterx-", 0) == 0u);
        CHECK(std::filesystem::file_size(e.path()) == 64u); // no file exceeds the cap
    }

    std::filesystem::remove_all(dir);
}

TEST_CASE("SbfWriter: EndSegmentStartsANewFile") {
    const auto dir = Scratch("segment");
    asterx::SbfWriter::Config cfg;
    cfg.output_dir = dir;
    asterx::SbfWriter w(cfg);

    CHECK(w.WriteBlock(Block(16)));
    CHECK(w.Stats().files_opened == 1u);
    w.EndSegment();
    CHECK(w.WriteBlock(Block(16)));
    CHECK(w.Stats().files_opened == 2u);
    w.close();
    CHECK(CountSbfFiles(dir) == 2u);

    // end_segment on a closed writer is a no-op, not a new empty file
    w.EndSegment();
    CHECK(CountSbfFiles(dir) == 2u);

    std::filesystem::remove_all(dir);
}

TEST_CASE("SbfWriter: ReportsFailureWhenTheOutputDirectoryDisappears") {
    const auto dir = Scratch("vanish");
    asterx::SbfWriter::Config cfg;
    cfg.output_dir = dir;
    cfg.rotate_bytes = 64; // one block per file, so the next write must open a new one
    asterx::SbfWriter w(cfg);

    CHECK(w.WriteBlock(Block(64)));
    std::filesystem::remove_all(dir);

    // The block itself may still reach the unlinked file descriptor, but the
    // rotation that follows cannot open a new file: the caller must be told.
    CHECK_FALSE(w.WriteBlock(Block(64)));
    CHECK_FALSE(w.WriteBlock(Block(64)));
}

TEST_CASE("SbfWriter: EmptyBlocksAreIgnored") {
    const auto dir = Scratch("empty");
    asterx::SbfWriter::Config cfg;
    cfg.output_dir = dir;
    asterx::SbfWriter w(cfg);

    CHECK(w.WriteBlock(QByteArray{}));
    CHECK(w.Stats().files_opened == 0u);
    CHECK(w.Stats().records_written == 0u);

    std::filesystem::remove_all(dir);
}
