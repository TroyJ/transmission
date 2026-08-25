// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>

#include <libtransmission/cache.h>
#include <libtransmission/session.h>
#include <libtransmission/torrent.h>

#include "test-fixtures.h"

namespace libtransmission::test
{

using TorrentTest = SessionTest;

namespace
{
auto constexpr TorFilenames = std::array{
    "Android-x86 8.1 r6 iso.torrent"sv,
    "debian-11.2.0-amd64-DVD-1.iso.torrent"sv,
    "ubuntu-18.04.6-desktop-amd64.iso.torrent"sv,
    "ubuntu-20.04.4-desktop-amd64.iso.torrent"sv,
};
}

TEST_F(TorrentTest, queueMoveUp)
{
    static constexpr auto ExpectedQueuePosition = std::array{ 0, 1, 3, 2 };
    auto ctor = tr_ctor{ session_ };
    auto torrents = std::array<tr_torrent*, TorFilenames.size()>{};
    std::transform(
        TorFilenames.begin(),
        TorFilenames.end(),
        torrents.begin(),
        [this](auto const filename) { return torrentInitFromFile(filename); });
    auto const move_torrents = std::array{ torrents[0], torrents[1], torrents[3] };

    // Pre-test sanity checks
    for (size_t i = 0; i < torrents.size(); ++i)
    {
        ASSERT_EQ(i, torrents[i]->queue_position());
        ASSERT_EQ(i + 1U, torrents[i]->id());
    }

    tr_torrentsQueueMoveUp(move_torrents.data(), move_torrents.size());

    for (size_t i = 0; i < ExpectedQueuePosition.size(); ++i)
    {
        EXPECT_EQ(ExpectedQueuePosition[i], torrents[i]->queue_position()) << i;
    }
}

TEST_F(TorrentTest, queueMoveDown)
{
    static constexpr auto ExpectedQueuePosition = std::array{ 1, 0, 2, 3 };
    auto ctor = tr_ctor{ session_ };
    auto torrents = std::array<tr_torrent*, TorFilenames.size()>{};
    std::transform(
        TorFilenames.begin(),
        TorFilenames.end(),
        torrents.begin(),
        [this](auto const filename) { return torrentInitFromFile(filename); });
    auto const move_torrents = std::array{ torrents[0], torrents[2], torrents[3] };

    // Pre-test sanity checks
    for (size_t i = 0; i < torrents.size(); ++i)
    {
        ASSERT_EQ(i, torrents[i]->queue_position());
        ASSERT_EQ(i + 1U, torrents[i]->id());
    }

    tr_torrentsQueueMoveDown(move_torrents.data(), move_torrents.size());

    for (size_t i = 0; i < ExpectedQueuePosition.size(); ++i)
    {
        EXPECT_EQ(ExpectedQueuePosition[i], torrents[i]->queue_position()) << i;
    }
}

TEST_F(TorrentTest, queueMoveTop)
{
    static constexpr auto ExpectedQueuePosition = std::array{ 0, 3, 1, 2 };
    auto ctor = tr_ctor{ session_ };
    auto torrents = std::array<tr_torrent*, TorFilenames.size()>{};
    std::transform(
        TorFilenames.begin(),
        TorFilenames.end(),
        torrents.begin(),
        [this](auto const filename) { return torrentInitFromFile(filename); });
    auto const move_torrents = std::array{ torrents[0], torrents[2], torrents[3] };

    // Pre-test sanity checks
    for (size_t i = 0; i < torrents.size(); ++i)
    {
        ASSERT_EQ(i, torrents[i]->queue_position());
        ASSERT_EQ(i + 1U, torrents[i]->id());
    }

    tr_torrentsQueueMoveTop(move_torrents.data(), move_torrents.size());

    for (size_t i = 0; i < ExpectedQueuePosition.size(); ++i)
    {
        EXPECT_EQ(ExpectedQueuePosition[i], torrents[i]->queue_position()) << i;
    }
}

TEST_F(TorrentTest, queueMoveBottom)
{
    static constexpr auto ExpectedQueuePosition = std::array{ 1, 2, 0, 3 };
    auto ctor = tr_ctor{ session_ };
    auto torrents = std::array<tr_torrent*, TorFilenames.size()>{};
    std::transform(
        TorFilenames.begin(),
        TorFilenames.end(),
        torrents.begin(),
        [this](auto const filename) { return torrentInitFromFile(filename); });
    auto const move_torrents = std::array{ torrents[0], torrents[1], torrents[3] };

    // Pre-test sanity checks
    for (size_t i = 0; i < torrents.size(); ++i)
    {
        ASSERT_EQ(i, torrents[i]->queue_position());
        ASSERT_EQ(i + 1U, torrents[i]->id());
    }

    tr_torrentsQueueMoveBottom(move_torrents.data(), move_torrents.size());

    for (size_t i = 0; i < ExpectedQueuePosition.size(); ++i)
    {
        EXPECT_EQ(ExpectedQueuePosition[i], torrents[i]->queue_position()) << i;
    }
}

// Regression: a piece that completes by downloading must be hashed
// off the session thread and, when it passes, recorded in
// checked_pieces_ so the resume file persists `progress.pieces`.
TEST_F(TorrentTest, downloadedPieceIsCheckedAndRecorded)
{
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Partial);
    EXPECT_NE(nullptr, tor);

    // the partial zero torrent is missing exactly one piece
    auto missing = std::optional<tr_piece_index_t>{};
    for (tr_piece_index_t piece = 0, n = tor->piece_count(); piece < n; ++piece)
    {
        if (!tor->has_piece(piece))
        {
            EXPECT_FALSE(missing.has_value());
            missing = piece;
        }
    }
    ASSERT_TRUE(missing.has_value());
    auto const piece = *missing;

    // simulate the state the bug leaves behind: the piece is unchecked
    // (e.g. loaded from a resume file with `progress.pieces = none`)
    {
        auto done = std::atomic<bool>{ false };
        session_->run_in_session_thread(
            [&]()
            {
                tor->checked_pieces_.set(piece, false);
                done = true;
            });
        EXPECT_TRUE(waitFor([&done]() { return done.load(); }, 5000));
    }
    EXPECT_FALSE(tor->is_piece_checked(piece));

    // feed the piece's blocks in through the write cache, like a peer would
    auto const [begin, end] = tor->block_span_for_piece(piece);
    for (tr_block_index_t block = begin; block < end; ++block)
    {
        auto done = std::atomic<bool>{ false };
        session_->run_in_session_thread(
            [&]()
            {
                auto buf = std::make_unique<Cache::BlockData>(tor->block_size(block));
                std::fill_n(std::data(*buf), std::size(*buf), '\0');
                session_->cache->write_block(tor->id(), block, std::move(buf));
                tor->on_block_received(block);
                done = true;
            });
        EXPECT_TRUE(waitFor([&done]() { return done.load(); }, 5000));
    }

    // has_piece() is true as soon as the last block lands...
    EXPECT_TRUE(tor->has_piece(piece));

    // ...and the off-thread check must then record the piece as verified
    EXPECT_TRUE(waitFor([tor, piece]() { return tor->is_piece_checked(piece); }, 5000));
    EXPECT_TRUE(tor->has_piece(piece));
    EXPECT_EQ(0, tr_torrentStat(tor)->leftUntilDone);

    // and the torrent must end up complete
    EXPECT_TRUE(waitFor([tor]() { return tr_torrentStat(tor)->activity != TR_STATUS_DOWNLOAD && tor->is_done(); }, 5000));

    tr_torrentRemove(tor, true, nullptr, nullptr);
}

// A downloaded piece whose bytes are wrong must fail the check and be
// dropped (not falsely marked verified).
TEST_F(TorrentTest, downloadedCorruptPieceIsRejected)
{
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Partial);
    EXPECT_NE(nullptr, tor);

    auto missing = std::optional<tr_piece_index_t>{};
    for (tr_piece_index_t piece = 0, n = tor->piece_count(); piece < n; ++piece)
    {
        if (!tor->has_piece(piece))
        {
            missing = piece;
        }
    }
    ASSERT_TRUE(missing.has_value());
    auto const piece = *missing;

    auto n_failed = std::atomic<int>{ 0 };
    auto tag = tor->got_bad_piece_.observe([&n_failed](tr_torrent*, tr_piece_index_t) { ++n_failed; });

    auto const [begin, end] = tor->block_span_for_piece(piece);
    for (tr_block_index_t block = begin; block < end; ++block)
    {
        auto done = std::atomic<bool>{ false };
        session_->run_in_session_thread(
            [&]()
            {
                auto buf = std::make_unique<Cache::BlockData>(tor->block_size(block));
                std::fill_n(std::data(*buf), std::size(*buf), 'x'); // wrong bytes
                session_->cache->write_block(tor->id(), block, std::move(buf));
                tor->on_block_received(block);
                done = true;
            });
        EXPECT_TRUE(waitFor([&done]() { return done.load(); }, 5000));
    }

    EXPECT_TRUE(waitFor([&n_failed]() { return n_failed.load() > 0; }, 5000));
    EXPECT_FALSE(tor->has_piece(piece));

    tr_torrentRemove(tor, true, nullptr, nullptr);
}

} // namespace libtransmission::test
