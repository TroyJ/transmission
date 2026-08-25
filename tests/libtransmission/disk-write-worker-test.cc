// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <libtransmission/transmission.h>

#include <libtransmission/disk-write-worker.h>
#include <libtransmission/file.h>
#include <libtransmission/tr-strbuf.h>

#include "gtest/gtest.h"
#include "test-fixtures.h"

using namespace std::literals;

using DiskWriteWorkerTest = libtransmission::test::SandboxedTest;

namespace
{

[[nodiscard]] tr_sys_file_t open_rw(std::string const& path)
{
    return tr_sys_file_open(path.c_str(), TR_SYS_FILE_READ | TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE, 0600);
}

[[nodiscard]] std::string read_all(std::string const& path)
{
    auto const fd = tr_sys_file_open(path.c_str(), TR_SYS_FILE_READ, 0600);
    if (fd == TR_BAD_SYS_FILE)
    {
        return {};
    }

    auto buf = std::string{};
    buf.resize(4096);
    auto n_read = uint64_t{};
    auto const ok = tr_sys_file_read(fd, std::data(buf), std::size(buf), &n_read);
    tr_sys_file_close(fd);
    buf.resize(ok ? n_read : 0U);
    return buf;
}

[[nodiscard]] std::vector<uint8_t> bytes_of(std::string_view sv)
{
    return { std::begin(sv), std::end(sv) };
}

} // namespace

TEST_F(DiskWriteWorkerTest, runJobWritesChunksAndClosesThem)
{
    auto const path = tr_pathbuf{ sandboxDir(), "/one.dat" }.sv();
    auto const filename = std::string{ path };

    auto job = tr_disk_write_worker::Job{};
    job.data = bytes_of("Hello, World!"sv);
    auto const fd = open_rw(filename);
    ASSERT_NE(TR_BAD_SYS_FILE, fd);
    job.chunks.push_back({ fd, 0U, std::size(job.data) });

    EXPECT_EQ(0, tr_disk_write_worker::run_job(job));
    EXPECT_EQ("Hello, World!"sv, read_all(filename));

    // the job owns the descriptors and must have closed them
    EXPECT_EQ(TR_BAD_SYS_FILE, job.chunks.front().fd);
}

TEST_F(DiskWriteWorkerTest, runJobSplitsAcrossFilesInOrder)
{
    auto const a = std::string{ tr_pathbuf{ sandboxDir(), "/a.dat" }.sv() };
    auto const b = std::string{ tr_pathbuf{ sandboxDir(), "/b.dat" }.sv() };

    auto job = tr_disk_write_worker::Job{};
    job.data = bytes_of("ABCDEFGH"sv);
    job.chunks.push_back({ open_rw(a), 0U, 3U }); // "ABC"
    job.chunks.push_back({ open_rw(b), 4U, 5U }); // "DEFGH" at offset 4

    EXPECT_EQ(0, tr_disk_write_worker::run_job(job));
    EXPECT_EQ("ABC"sv, read_all(a));

    auto const b_contents = read_all(b);
    ASSERT_EQ(9U, std::size(b_contents));
    EXPECT_EQ("DEFGH"sv, std::string_view{ b_contents }.substr(4));
}

TEST_F(DiskWriteWorkerTest, runJobRejectsChunksLongerThanTheBuffer)
{
    auto const path = std::string{ tr_pathbuf{ sandboxDir(), "/short.dat" }.sv() };

    auto job = tr_disk_write_worker::Job{};
    job.data = bytes_of("abc"sv);
    job.chunks.push_back({ open_rw(path), 0U, 4096U }); // claims more than we have

    EXPECT_NE(0, tr_disk_write_worker::run_job(job));
    EXPECT_TRUE(std::empty(read_all(path)));
}

TEST_F(DiskWriteWorkerTest, runJobReportsABadDescriptor)
{
    auto job = tr_disk_write_worker::Job{};
    job.data = bytes_of("abc"sv);
    job.chunks.push_back({ TR_BAD_SYS_FILE, 0U, 3U });

    EXPECT_NE(0, tr_disk_write_worker::run_job(job));
}

TEST_F(DiskWriteWorkerTest, writesHappenInFifoOrder)
{
    // Two jobs writing to the same region: the second must win, which is only
    // true if the worker preserves submission order.
    auto const path = std::string{ tr_pathbuf{ sandboxDir(), "/ordered.dat" }.sv() };
    auto order = std::vector<int>{};
    auto mutex = std::mutex{};

    auto worker = tr_disk_write_worker{};
    for (auto i = 0; i < 16; ++i)
    {
        auto job = tr_disk_write_worker::Job{};
        job.data = bytes_of(i % 2 == 0 ? "even"sv : "odd_"sv);
        job.chunks.push_back({ open_rw(path), 0U, 4U });
        job.on_done = [&order, &mutex, i](int err)
        {
            EXPECT_EQ(0, err);
            auto const lock = std::scoped_lock{ mutex };
            order.push_back(i);
        };
        worker.add(std::move(job));
    }

    worker.drain();

    ASSERT_EQ(16U, std::size(order));
    for (auto i = 0; i < 16; ++i)
    {
        EXPECT_EQ(i, order[i]) << "job " << i << " completed out of order";
    }
    EXPECT_EQ("odd_"sv, read_all(path)); // the last job submitted
}

TEST_F(DiskWriteWorkerTest, drainWaitsForEverythingQueued)
{
    auto const path = std::string{ tr_pathbuf{ sandboxDir(), "/drain.dat" }.sv() };
    auto done = std::atomic<int>{ 0 };

    auto worker = tr_disk_write_worker{};
    static auto constexpr NumJobs = 32;
    for (auto i = 0; i < NumJobs; ++i)
    {
        auto job = tr_disk_write_worker::Job{};
        job.data = bytes_of("payload"sv);
        job.chunks.push_back({ open_rw(path), static_cast<uint64_t>(i * 8), 7U });
        job.on_done = [&done](int /*err*/)
        {
            ++done;
        };
        worker.add(std::move(job));
    }

    worker.drain();

    EXPECT_EQ(NumJobs, done.load());
    EXPECT_EQ(0U, worker.pending_bytes());
    EXPECT_EQ(0U, worker.pending_jobs());
}

TEST_F(DiskWriteWorkerTest, destructorDoesNotDropQueuedData)
{
    // Data handed to the worker has already been dropped from the cache, so
    // losing it on shutdown would lose downloaded blocks.
    auto const path = std::string{ tr_pathbuf{ sandboxDir(), "/shutdown.dat" }.sv() };
    static auto constexpr NumJobs = 64;
    auto done = std::atomic<int>{ 0 };

    {
        auto worker = tr_disk_write_worker{};
        for (auto i = 0; i < NumJobs; ++i)
        {
            auto job = tr_disk_write_worker::Job{};
            job.data = bytes_of("xyz"sv);
            job.chunks.push_back({ open_rw(path), static_cast<uint64_t>(i * 4), 3U });
            job.on_done = [&done](int err)
            {
                EXPECT_EQ(0, err);
                ++done;
            };
            worker.add(std::move(job));
        }
    } // dtor must drain, not discard

    EXPECT_EQ(NumJobs, done.load());
    EXPECT_EQ(static_cast<size_t>(NumJobs * 4 - 1), std::size(read_all(path)));
}

TEST_F(DiskWriteWorkerTest, pendingBytesTracksAcceptedWork)
{
    auto worker = tr_disk_write_worker{};
    EXPECT_EQ(0U, worker.pending_bytes());

    auto const path = std::string{ tr_pathbuf{ sandboxDir(), "/pending.dat" }.sv() };
    auto job = tr_disk_write_worker::Job{};
    job.data = bytes_of("0123456789"sv);
    job.chunks.push_back({ open_rw(path), 0U, 10U });
    worker.add(std::move(job));

    worker.drain();
    EXPECT_EQ(0U, worker.pending_bytes());
}

TEST_F(DiskWriteWorkerTest, runJobOpensByPathCreatingParentsAndClosesWhenDone)
{
    auto const path = tr_pathbuf{ sandboxDir(), "/a/b/c.part"sv };
    static auto constexpr Payload = "hello, worker"sv;

    auto job = tr_disk_write_worker::Job{};
    job.data.assign(std::begin(Payload), std::end(Payload));
    auto chunk = tr_disk_write_worker::Chunk{};
    chunk.path = std::string{ path.sv() };
    chunk.file_offset = 4U;
    chunk.length = std::size(Payload);
    chunk.file_size = 32U;
    job.chunks.push_back(std::move(chunk));

    EXPECT_FALSE(tr_sys_path_exists(path));
    EXPECT_EQ(0, tr_disk_write_worker::run_job(job));
    EXPECT_EQ(TR_BAD_SYS_FILE, job.chunks.front().fd) << "closed after the write";

    auto const info = tr_sys_path_get_info(path);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(4U + std::size(Payload), info->size);

    auto buf = std::array<char, 64>{};
    auto const fd = tr_sys_file_open(path, TR_SYS_FILE_READ, 0);
    ASSERT_NE(TR_BAD_SYS_FILE, fd);
    auto n_read = uint64_t{};
    EXPECT_TRUE(tr_sys_file_read_at(fd, std::data(buf), std::size(Payload), 4U, &n_read));
    tr_sys_file_close(fd);
    EXPECT_EQ(Payload, std::string_view(std::data(buf), n_read));
}

TEST_F(DiskWriteWorkerTest, abandonDropsTheQueueAndTheDestructorDoesNotWait)
{
    auto worker = std::make_unique<tr_disk_write_worker>();
    worker->set_paused(true);

    auto job = tr_disk_write_worker::Job{};
    job.data.assign(16U, uint8_t{ 1 });
    job.chunks.push_back({ TR_BAD_SYS_FILE, 0U, 16U });
    worker->add(std::move(job));
    EXPECT_EQ(1U, worker->pending_jobs());

    EXPECT_FALSE(worker->drain_for(std::chrono::milliseconds{ 50 })) << "nothing can drain while paused... ";

    EXPECT_EQ(1U, worker->abandon());
    EXPECT_TRUE(worker->is_abandoned());
    EXPECT_EQ(0U, worker->pending_bytes());

    // ...and after abandon, new jobs are discarded and teardown is immediate.
    auto dropped = tr_disk_write_worker::Job{};
    dropped.chunks.push_back({ TR_BAD_SYS_FILE, 0U, 0U });
    worker->add(std::move(dropped));
    EXPECT_EQ(0U, worker->pending_jobs());
    worker.reset();
}
