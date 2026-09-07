// SPDX-License-Identifier: BSD-3-Clause
// The write queue decouples the SsnRx/Qt thread from the disk: blocks land in
// order, the disk falling behind beyond the budget REFUSES (never drops) a
// block, and a failed write surfaces on the writer thread through the callback.
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <QByteArray>
#include <doctest/doctest.h>

#include "sbf_write_queue.h"

namespace {
    std::filesystem::path Scratch(const std::string &name) {
        const auto p = std::filesystem::temp_directory_path() /
                       ("asterx_queue_" + std::to_string(::getpid()) + "_" + name);
        std::filesystem::remove_all(p);
        return p;
    }

    asterx::SbfWriter::Config Config(const std::filesystem::path &dir) {
        asterx::SbfWriter::Config c;
        c.output_dir = dir;
        c.file_prefix = "q";
        c.rotate_bytes = 1u << 20;
        c.rotate_interval = std::chrono::hours(1);
        return c;
    }

    bool WaitFor(const std::function<bool()> &done, int timeout_ms = 3000) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return done();
    }

    // The single .sbf file's content
    std::string ReadSbf(const std::filesystem::path &dir) {
        for (const auto &e: std::filesystem::directory_iterator(dir)) {
            if (e.path().extension() == ".sbf") {
                std::ifstream in(e.path(), std::ios::binary);
                return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            }
        }
        return {};
    }
} // namespace

TEST_CASE("SbfWriteQueue: BlocksLandOnDiskInOrder") {
    const auto dir = Scratch("order");
    asterx::SbfWriteQueue queue(Config(dir), 1u << 20);
    queue.Start();
    std::string expected;
    for (int i = 0; i < 100; ++i) {
        const QByteArray block(100, static_cast<char>('a' + i % 26));
        REQUIRE(queue.Enqueue(block));
        expected.append(block.constData(), static_cast<std::size_t>(block.size()));
    }
    CHECK(queue.Close());
    CHECK_FALSE(queue.Failed());
    CHECK(queue.LastError().empty());

    const auto s = queue.GetStats();
    CHECK(s.records_written == 100);
    CHECK(s.bytes_written == 10000);
    CHECK(s.files_opened == 1);
    CHECK(s.pending_bytes == 0);
    CHECK(s.max_pending_bytes <= 10000);
    CHECK(ReadSbf(dir) == expected);
    std::filesystem::remove_all(dir);
}

TEST_CASE("SbfWriteQueue: BudgetOverflowRefusesTheBlockAndLatchesFailure") {
    // The thread is deliberately NOT started, so nothing drains: the second
    // block does not fit the 150 B budget. The first one is still written by
    // Close() — accepted blocks are never lost, only the refused one is missing.
    const auto dir = Scratch("overflow");
    asterx::SbfWriteQueue queue(Config(dir), 150);
    CHECK(queue.Enqueue(QByteArray(100, 'x')));
    CHECK_FALSE(queue.Enqueue(QByteArray(100, 'y')));
    CHECK(queue.Failed());
    CHECK(queue.LastError().find("budget") != std::string::npos);
    CHECK_FALSE(queue.Close());
    CHECK(queue.GetStats().records_written == 1);
    CHECK(ReadSbf(dir) == std::string(100, 'x'));
    std::filesystem::remove_all(dir);
}

TEST_CASE("SbfWriteQueue: AWriteFailureReachesTheCallbackOnceAndLatches") {
    const auto dir = Scratch("disk");
    std::atomic<int> notified{0};
    asterx::SbfWriteQueue queue(Config(dir), 1u << 20, [&notified] { ++notified; });
    queue.Start();
    std::filesystem::remove_all(dir); // the output directory disappears before the first write
    CHECK(queue.Enqueue(QByteArray(64, 'z'))); // accepted: the failure is only known once the writer tries
    REQUIRE(WaitFor([&] { return queue.Failed(); }));
    CHECK(WaitFor([&] { return notified.load() == 1; }));
    CHECK_FALSE(queue.Enqueue(QByteArray(64, 'z'))); // refused after the failure
    CHECK_FALSE(queue.Close());
    CHECK(notified.load() == 1); // the close failure does not notify again
    std::filesystem::remove_all(dir);
}

TEST_CASE("SbfWriteQueue: CloseWithoutStartWritesTheQueuedBlocks") {
    const auto dir = Scratch("nostart");
    asterx::SbfWriteQueue queue(Config(dir), 1u << 20);
    CHECK(queue.Enqueue(QByteArray(10, 'a')));
    CHECK(queue.Enqueue(QByteArray(10, 'b')));
    CHECK(queue.Close());
    CHECK(ReadSbf(dir) == std::string(10, 'a') + std::string(10, 'b'));
    CHECK(queue.GetStats().pending_bytes == 0);
    std::filesystem::remove_all(dir);
}
