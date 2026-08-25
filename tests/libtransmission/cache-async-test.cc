// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <libtransmission/transmission.h>

#include <libtransmission/cache.h>
#include <libtransmission/file.h>
#include <libtransmission/open-files.h>
#include <libtransmission/session.h>
#include <libtransmission/torrent.h>
#include <libtransmission/values.h>

#include "gtest/gtest.h"
#include "test-fixtures.h"

/**
 * Covers the properties that make the asynchronous write path safe.
 *
 * The write itself now happens on a background thread, so a block can be
 * "written" from the caller's point of view while its bytes are still in
 * flight. Everything that reads a block -- serving a peer, hashing a piece --
 * has to see it anyway. See docs/async-write-path-2a.md.
 */
namespace libtransmission::test
{

using CacheAsyncTest = SessionTest;

namespace
{

auto constexpr TorFilename = "debian-11.2.0-amd64-DVD-1.iso.torrent"sv;

// Distinctive per-block bytes, so a read that silently returns zeroes -- a
// hole where the data has not landed yet -- fails instead of passing.
[[nodiscard]] std::unique_ptr<Cache::BlockData> make_block(tr_torrent const* tor, tr_block_index_t block)
{
    auto buf = std::make_unique<Cache::BlockData>(tor->block_size(block));
    std::fill_n(std::data(*buf), std::size(*buf), static_cast<uint8_t>('A' + (block % 26U)));
    return buf;
}

template<typename Func>
void in_session_thread(tr_session* session, Func&& func)
{
    auto done = std::atomic<bool>{ false };
    session->run_in_session_thread(
        [&]()
        {
            func();
            done = true;
        });
    EXPECT_TRUE(waitFor([&done]() { return done.load(); }, 5000));
}

} // namespace

TEST_F(CacheAsyncTest, blockIsReadableWhileItsWriteIsStillInFlight)
{
    auto* const tor = torrentInitFromFile(TorFilename);
    ASSERT_NE(nullptr, tor);

    static auto constexpr Block = tr_block_index_t{ 0 };
    auto const expected = static_cast<uint8_t>('A');

    // Park the writes so the flush cannot complete behind our back.
    session_->cache->set_write_paused(true);

    in_session_thread(
        session_,
        [this, tor]()
        {
            session_->cache->write_block(tor->id(), Block, make_block(tor, Block));
            // Force the flush: the block leaves the writable cache and is
            // handed to the worker, which is paused and holding it.
            EXPECT_EQ(0, session_->cache->flush_torrent(tor->id()));
        });

    EXPECT_LT(0U, session_->cache->pending_write_bytes()) << "the write should still be in flight";

    // The acceptance criterion: still readable, with the right bytes.
    auto buf = std::vector<uint8_t>(tor->block_size(Block), 0U);
    in_session_thread(
        session_,
        [this, tor, &buf]()
        { EXPECT_TRUE(session_->cache->copy_cached_block(*tor, tor->block_loc(Block), std::size(buf), std::data(buf))); });
    EXPECT_EQ(expected, buf.front());
    EXPECT_EQ(expected, buf.back());

    // read_block() must serve it too, rather than reading a hole from disk.
    std::fill(std::begin(buf), std::end(buf), uint8_t{ 0 });
    in_session_thread(
        session_,
        [this, tor, &buf]()
        { EXPECT_EQ(0, session_->cache->read_block(*tor, tor->block_loc(Block), std::size(buf), std::data(buf))); });
    EXPECT_EQ(expected, buf.front());
    EXPECT_EQ(expected, buf.back());

    session_->cache->set_write_paused(false);
    in_session_thread(session_, [this]() { session_->cache->drain(); });

    EXPECT_EQ(0U, session_->cache->pending_write_bytes());
}

TEST_F(CacheAsyncTest, drainPutsTheBytesOnDisk)
{
    auto* const tor = torrentInitFromFile(TorFilename);
    ASSERT_NE(nullptr, tor);

    static auto constexpr NumBlocks = tr_block_index_t{ 8 };

    in_session_thread(
        session_,
        [this, tor]()
        {
            for (tr_block_index_t block = 0; block < NumBlocks; ++block)
            {
                session_->cache->write_block(tor->id(), block, make_block(tor, block));
            }
            EXPECT_EQ(0, session_->cache->flush_torrent(tor->id()));
            session_->cache->drain();
        });

    EXPECT_EQ(0U, session_->cache->pending_write_bytes());

    // Nothing is cached any more, so these reads come off the disk.
    for (tr_block_index_t block = 0; block < NumBlocks; ++block)
    {
        auto buf = std::vector<uint8_t>(tor->block_size(block), 0U);
        in_session_thread(
            session_,
            [this, tor, block, &buf]()
            { EXPECT_EQ(0, session_->cache->read_block(*tor, tor->block_loc(block), std::size(buf), std::data(buf))); });
        EXPECT_EQ(static_cast<uint8_t>('A' + (block % 26U)), buf.front()) << "block " << block;
        EXPECT_EQ(static_cast<uint8_t>('A' + (block % 26U)), buf.back()) << "block " << block;
    }
}

TEST_F(CacheAsyncTest, writesDoNotBlockTheSessionThreadWhileTheDiskIsStalled)
{
    // The whole point of 2a: with the disk wedged, the session thread must
    // still be answering. A paused worker is a disk that never completes.
    auto* const tor = torrentInitFromFile(TorFilename);
    ASSERT_NE(nullptr, tor);

    session_->cache->set_write_paused(true);

    static auto constexpr NumBlocks = tr_block_index_t{ 64 };
    in_session_thread(
        session_,
        [this, tor]()
        {
            for (tr_block_index_t block = 0; block < NumBlocks; ++block)
            {
                session_->cache->write_block(tor->id(), block, make_block(tor, block));
            }
            EXPECT_EQ(0, session_->cache->flush_torrent(tor->id()));
        });

    EXPECT_LT(0U, session_->cache->pending_write_bytes());

    // If the writes were synchronous this would never come back.
    auto ran = std::atomic<bool>{ false };
    session_->run_in_session_thread([&ran]() { ran = true; });
    EXPECT_TRUE(waitFor([&ran]() { return ran.load(); }, 5000)) << "the session thread is blocked on the disk";

    session_->cache->set_write_paused(false);
    in_session_thread(session_, [this]() { session_->cache->drain(); });
}

TEST_F(CacheAsyncTest, backpressureIsReportedWhenTheWorkerFallsBehind)
{
    auto* const tor = torrentInitFromFile(TorFilename);
    ASSERT_NE(nullptr, tor);

    EXPECT_FALSE(session_->cache->is_write_backlogged());
    EXPECT_FALSE(session_->is_disk_write_backlogged());

    session_->cache->set_write_paused(true);

    // Enough blocks to bury the in-flight ceiling.
    auto const n_blocks = static_cast<tr_block_index_t>(Cache::MaxInFlightBytes / tor->block_size(0) + 16U);
    in_session_thread(
        session_,
        [this, tor, n_blocks]()
        {
            for (tr_block_index_t block = 0; block < n_blocks; ++block)
            {
                session_->cache->write_block(tor->id(), block, make_block(tor, block));
            }
            EXPECT_EQ(0, session_->cache->flush_torrent(tor->id()));
        });

    EXPECT_TRUE(session_->cache->is_write_backlogged());
    EXPECT_TRUE(session_->is_disk_write_backlogged()) << "peers should stop being asked for blocks";

    session_->cache->set_write_paused(false);
    in_session_thread(session_, [this]() { session_->cache->drain(); });

    EXPECT_FALSE(session_->cache->is_write_backlogged());
}

TEST_F(CacheAsyncTest, aZeroSizedCacheStillWritesOffThread)
{
    // cache-size-mb=0 means "hold nothing back", not "write on the session
    // thread". It used to mean the latter.
    auto* const tor = torrentInitFromFile(TorFilename);
    ASSERT_NE(nullptr, tor);

    in_session_thread(
        session_,
        [this]() { EXPECT_EQ(0, session_->cache->set_limit(Values::Memory{ 0U, Values::Memory::Units::Bytes })); });

    session_->cache->set_write_paused(true);
    in_session_thread(session_, [this, tor]() { session_->cache->write_block(tor->id(), 0, make_block(tor, 0)); });

    EXPECT_LT(0U, session_->cache->pending_write_bytes()) << "the write went to the worker, not the session thread";

    session_->cache->set_write_paused(false);
    in_session_thread(session_, [this]() { session_->cache->drain(); });
}

TEST_F(CacheAsyncTest, fileClosesGoToTheWorkerNotTheSessionThread)
{
    // close() flushes the file's dirty pages; on a stalled volume it has been
    // measured at 144 seconds. It must not happen on the session thread.
    auto* const tor = torrentInitFromFile(TorFilename);
    ASSERT_NE(nullptr, tor);

    // Get a file open in the pool. Writes no longer go through the pool (the
    // worker opens by path), so open one the way a read does: put the file on
    // disk first, then have the pool open it read-only.
    in_session_thread(
        session_,
        [this, tor]()
        {
            session_->cache->write_block(tor->id(), 0, make_block(tor, 0));
            EXPECT_EQ(0, session_->cache->flush_torrent(tor->id()));
            session_->cache->drain();
            auto const path = tor->found_file_path(0);
            ASSERT_TRUE(path.has_value());
            EXPECT_TRUE(
                session_->openFiles().get(tor->id(), 0, false, *path, tr_open_files::Preallocation::None, tor->file_size(0)));
        });

    session_->cache->set_write_paused(true);

    // Closing the torrent's files must hand the descriptors off rather than
    // closing them here, so the work shows up as pending on the worker.
    in_session_thread(session_, [this, tor]() { session_->openFiles().close_torrent(tor->id()); });

    EXPECT_LT(0U, session_->cache->pending_jobs()) << "the close should have been queued, not run inline";

    session_->cache->set_write_paused(false);
    in_session_thread(session_, [this]() { session_->cache->drain(); });

    EXPECT_EQ(0U, session_->cache->pending_jobs());
}

TEST_F(CacheAsyncTest, continuationWaitsForThePendingWritesWithoutBlocking)
{
    // What replaced the drain() at file completion. The callback must not run
    // until the file's writes have landed -- the `.part` rename and the mtime
    // read both depend on it -- but the session thread must not wait either.
    auto* const tor = torrentInitFromFile(TorFilename);
    ASSERT_NE(nullptr, tor);

    session_->cache->set_write_paused(true);

    auto fired = std::atomic<bool>{ false };
    in_session_thread(
        session_,
        [this, tor, &fired]()
        {
            for (tr_block_index_t block = 0; block < 8; ++block)
            {
                session_->cache->write_block(tor->id(), block, make_block(tor, block));
            }
            session_->close_torrent_file_async(*tor, 0, [&fired]() { fired = true; });
        });

    // The writes are parked, so the continuation must still be waiting...
    EXPECT_FALSE(fired.load()) << "the continuation ran before its writes landed";
    EXPECT_LT(0U, session_->cache->pending_jobs());

    // ...but the session thread is not waiting with it.
    auto ran = std::atomic<bool>{ false };
    session_->run_in_session_thread([&ran]() { ran = true; });
    EXPECT_TRUE(waitFor([&ran]() { return ran.load(); }, 5000)) << "the session thread is blocked";
    EXPECT_FALSE(fired.load());

    // Let the disk go, and only now should it fire.
    session_->cache->set_write_paused(false);
    EXPECT_TRUE(waitFor([&fired]() { return fired.load(); }, 5000)) << "the continuation never ran";

    in_session_thread(session_, [this]() { session_->cache->drain(); });
}

TEST_F(CacheAsyncTest, removingATorrentWithDataDoesNotWaitForTheDisk)
{
    auto* const tor = torrentInitFromFile(TorFilename);
    ASSERT_NE(nullptr, tor);
    auto const tor_id = tor->id();

    // Put the file on disk, then park a second flush on the "disk"...
    in_session_thread(
        session_,
        [this, tor]()
        {
            session_->cache->write_block(tor->id(), 0, make_block(tor, 0));
            EXPECT_EQ(0, session_->cache->flush_torrent(tor->id()));
            session_->cache->drain();
        });
    session_->cache->set_write_paused(true);
    in_session_thread(
        session_,
        [this, tor]()
        {
            session_->cache->write_block(tor->id(), 1, make_block(tor, 1));
            EXPECT_EQ(0, session_->cache->flush_torrent(tor->id()));
        });
    EXPECT_LT(0U, session_->cache->pending_write_bytes());

    auto const path = std::string{ tor->found_file_path(0).value_or("") };
    ASSERT_FALSE(std::empty(path));
    ASSERT_TRUE(tr_sys_path_exists(path.c_str()));

    // ...and remove the torrent with its data. This used to drain the worker
    // on the session thread: with the disk stalled, the app stopped here.
    tr_torrentRemove(tor, true, nullptr, nullptr);

    // The torrent is gone from the session at once, disk or no disk...
    EXPECT_TRUE(waitFor([this, tor_id]() { return session_->torrents().get(tor_id) == nullptr; }, 5000));

    // ...and the session thread is still answering.
    auto ran = std::atomic<bool>{ false };
    session_->run_in_session_thread([&ran]() { ran = true; });
    EXPECT_TRUE(waitFor([&ran]() { return ran.load(); }, 5000)) << "the session thread is blocked on the disk";

    // The file is only deleted once the disk has caught up.
    EXPECT_TRUE(tr_sys_path_exists(path.c_str()));
    session_->cache->set_write_paused(false);
    EXPECT_TRUE(waitFor([&path]() { return !tr_sys_path_exists(path.c_str()); }, 5000));
}

TEST_F(CacheAsyncTest, shutdownGivesUpOnAStalledDiskAndForgetsTheUnwrittenBlocks)
{
    // What quit does when the disk never answers: wait a bounded time, then
    // record which blocks did not land so the resume file does not claim them.
    auto* const tor = torrentInitFromFile(TorFilename);
    ASSERT_NE(nullptr, tor);

    session_->cache->set_write_paused(true);
    static auto constexpr NumBlocks = tr_block_index_t{ 4 };
    in_session_thread(
        session_,
        [this, tor]()
        {
            for (tr_block_index_t block = 0; block < NumBlocks; ++block)
            {
                session_->cache->write_block(tor->id(), block, make_block(tor, block));
                tor->on_block_received(block); // marks it had, as the peer path does
            }
            EXPECT_EQ(0, session_->cache->flush_torrent(tor->id()));
        });
    ASSERT_LE(NumBlocks, tor->block_span_for_piece(0).end);
    for (tr_block_index_t block = 0; block < NumBlocks; ++block)
    {
        EXPECT_TRUE(tor->has_block(block));
    }

    // drain_for() is a bounded wait; a paused worker is a disk that never answers.
    auto drained = true;
    in_session_thread(session_, [this, &drained]() { drained = session_->cache->drain_for(std::chrono::milliseconds{ 100 }); });
    EXPECT_FALSE(drained);

    auto abandoned = std::vector<std::pair<tr_torrent_id_t, tr_block_index_t>>{};
    in_session_thread(session_, [this, &abandoned]() { abandoned = session_->cache->abandon_pending(); });
    EXPECT_EQ(NumBlocks, std::size(abandoned));
    EXPECT_EQ(0U, session_->cache->pending_write_bytes());

    for (auto const& [tor_id, block] : abandoned)
    {
        EXPECT_EQ(tor->id(), tor_id);
        tor->forget_unwritten_block(block);
    }
    for (tr_block_index_t block = 0; block < NumBlocks; ++block)
    {
        EXPECT_FALSE(tor->has_block(block)) << "block " << block << " never reached the disk";
    }
}

} // namespace libtransmission::test
