// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <libtransmission/transmission.h>

#include <libtransmission/cache.h>
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

} // namespace libtransmission::test
