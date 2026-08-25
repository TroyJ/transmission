// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include <libtransmission/cache.h>
#include <libtransmission/file.h>
#include <libtransmission/torrent-ctor.h>
#include <libtransmission/tr-strbuf.h>
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

// Regression: files that are absent at load time (e.g. an unmounted
// volume) must not have their verification state wiped, or the next
// resume save persists `progress.pieces = none`.
TEST_F(TorrentTest, absentFilesKeepVerificationState)
{
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    EXPECT_NE(nullptr, tor);
    blockingTorrentVerify(tor);
    EXPECT_TRUE(tor->checked_pieces_.has_all());

    auto const saved_checked = tor->checked_pieces_;
    auto const saved_mtimes = tor->file_mtimes_;
    ASSERT_EQ(tor->file_count(), std::size(saved_mtimes));
    for (auto const mtime : saved_mtimes)
    {
        EXPECT_GT(mtime, 0);
    }

    auto const load = [&](std::vector<time_t> const& mtimes)
    {
        auto done = std::atomic<bool>{ false };
        session_->run_in_session_thread(
            [&]()
            {
                tr_torrent::ResumeHelper{ *tor }.load_checked_pieces(saved_checked, std::data(mtimes));
                done = true;
            });
        EXPECT_TRUE(waitFor([&done]() { return done.load(); }, 5000));
    };

    // hide the data, as if its volume were unmounted
    auto const dir = tr_pathbuf{ tr_sessionGetDownloadDir(session_), "/files-filled-with-zeroes"sv };
    auto const hidden = tr_pathbuf{ tr_sessionGetDownloadDir(session_), "/hidden"sv };
    ASSERT_TRUE(tr_sys_path_rename(dir, hidden));

    load(saved_mtimes);
    EXPECT_TRUE(tor->checked_pieces_.has_all()) << "absent files must keep their verification state";
    EXPECT_EQ(saved_mtimes, tor->file_mtimes_) << "absent files must keep their saved mtimes";
    EXPECT_FALSE(tor->any_local_data_found_at_load_);

    // bring it back unchanged: still verified
    ASSERT_TRUE(tr_sys_path_rename(hidden, dir));
    load(saved_mtimes);
    EXPECT_TRUE(tor->checked_pieces_.has_all());
    EXPECT_TRUE(tor->any_local_data_found_at_load_);

    // bring it back changed: that file's pieces are unset
    auto changed = saved_mtimes;
    changed[0] += 1;
    load(changed);
    EXPECT_FALSE(tor->checked_pieces_.has_all());
    auto const [begin, end] = tor->piece_span_for_file(0);
    for (auto piece = begin; piece < end; ++piece)
    {
        EXPECT_FALSE(tor->is_piece_checked(piece));
    }

    tr_torrentRemove(tor, true, nullptr, nullptr);
}

// Blocks served to peers must be read off the session thread: the first
// call queues a disk read, a later call is served from memory.
TEST_F(TorrentTest, readBlockForPeerIsServedOffThread)
{
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    EXPECT_NE(nullptr, tor);

    auto buf = std::vector<uint8_t>(tr_block_info::BlockSize, 0xAA);
    auto const read = [&](tr_block_index_t block) -> std::optional<bool>
    {
        auto result = std::optional<bool>{};
        auto done = std::atomic<bool>{ false };
        session_->run_in_session_thread(
            [&]()
            {
                result = tor->read_block_for_peer(tor->block_loc(block), tor->block_size(block), std::data(buf));
                done = true;
            });
        EXPECT_TRUE(waitFor([&done]() { return done.load(); }, 5000));
        return result;
    };

    // nothing is in the write cache, so the first call must defer
    EXPECT_FALSE(read(0).has_value());

    // ...and a later call is served from the prefetch
    auto served = std::optional<bool>{};
    EXPECT_TRUE(waitFor(
        [&]()
        {
            served = read(0);
            return served.has_value();
        },
        5000));
    EXPECT_EQ(true, served);
    EXPECT_TRUE(std::all_of(std::begin(buf), std::begin(buf) + tor->block_size(0), [](auto b) { return b == 0; }));

    // served entries are consumed: the next call for the same block defers again
    EXPECT_FALSE(read(0).has_value());

    // unreadable data is reported as false, not nullopt forever
    auto const dir = tr_pathbuf{ tr_sessionGetDownloadDir(session_), "/files-filled-with-zeroes"sv };
    auto const hidden = tr_pathbuf{ tr_sessionGetDownloadDir(session_), "/hidden"sv };
    ASSERT_TRUE(tr_sys_path_rename(dir, hidden));
    EXPECT_FALSE(read(1).has_value());
    served.reset();
    EXPECT_TRUE(waitFor(
        [&]()
        {
            served = read(1);
            return served.has_value();
        },
        5000));
    EXPECT_EQ(false, served);
    ASSERT_TRUE(tr_sys_path_rename(hidden, dir));

    tr_torrentRemove(tor, true, nullptr, nullptr);
}

// Add-time seed detection is optimistic: a torrent whose files look complete
// and untouched is marked complete immediately and piece 0 is probed off-thread.
// If the probe fails the torrent must be sent to a full verify.
class SeedProbeTest : public TorrentTest
{
protected:
    // returns the path of a saved copy of the .torrent file, with the data
    // files left in place and backdated so they look older than any add
    [[nodiscard]] std::string makeUntouchedCompleteData(bool corrupt_piece_zero)
    {
        auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
        EXPECT_NE(nullptr, tor);
        auto const saved = std::string{ tr_pathbuf{ sandboxDir(), "/saved.torrent"sv } };
        EXPECT_TRUE(tr_sys_path_copy(tor->torrent_file().c_str(), saved.c_str()));
        auto const n_files = tor->file_count();
        auto paths = std::vector<std::string>{};
        for (tr_file_index_t i = 0; i < n_files; ++i)
        {
            paths.emplace_back(tr_torrentFindFile(tor, i));
        }
        tr_torrentRemove(tor, false, nullptr, nullptr);
        EXPECT_TRUE(
            waitFor([&]() { return tr_sys_path_exists(paths.front()) && std::size(session_->torrents()) == 0U; }, 5000));

        if (corrupt_piece_zero)
        {
            auto const fd = tr_sys_file_open(paths.front().c_str(), TR_SYS_FILE_WRITE, 0);
            EXPECT_NE(TR_BAD_SYS_FILE, fd);
            auto const ch = 'x';
            EXPECT_TRUE(tr_sys_file_write(fd, &ch, 1, nullptr));
            tr_sys_file_close(fd);
        }

        auto const old_time = std::filesystem::file_time_type::clock::now() - std::chrono::hours{ 24 };
        for (auto const& path : paths)
        {
            std::filesystem::last_write_time(path, old_time);
        }
        return saved;
    }
};

TEST_F(SeedProbeTest, untouchedCompleteDataIsSeedWithoutVerify)
{
    auto const torrent_file = makeUntouchedCompleteData(false);

    auto* const ctor = tr_ctorNew(session_);
    EXPECT_TRUE(tr_ctorSetMetainfoFromFile(ctor, torrent_file.c_str(), nullptr));
    tr_ctorSetPaused(ctor, TR_FORCE, true);
    auto* const tor = tr_torrentNew(ctor, nullptr);
    tr_ctorFree(ctor);
    ASSERT_NE(nullptr, tor);

    // marked complete immediately, no synchronous hash at add time
    EXPECT_TRUE(tor->has_all());
    EXPECT_NE(TR_STATUS_CHECK, tr_torrentStat(tor)->activity);

    // and the off-thread probe confirms piece 0
    EXPECT_TRUE(waitFor([tor]() { return tor->is_piece_checked(0); }, 5000));
    EXPECT_TRUE(tor->has_all());
    EXPECT_NE(TR_STATUS_CHECK, tr_torrentStat(tor)->activity);
    EXPECT_NE(TR_STATUS_CHECK_WAIT, tr_torrentStat(tor)->activity);

    tr_torrentRemove(tor, true, nullptr, nullptr);
}

TEST_F(SeedProbeTest, corruptPieceZeroTriggersFullVerify)
{
    auto const torrent_file = makeUntouchedCompleteData(true);

    auto* const ctor = tr_ctorNew(session_);
    EXPECT_TRUE(tr_ctorSetMetainfoFromFile(ctor, torrent_file.c_str(), nullptr));
    tr_ctorSetPaused(ctor, TR_FORCE, true);
    auto* const tor = createTorrentAndWaitForVerifyDone(ctor); // the probe must trigger a verify
    tr_ctorFree(ctor);
    ASSERT_NE(nullptr, tor);

    EXPECT_FALSE(tor->has_piece(0));
    EXPECT_TRUE(tor->has_piece(1));
    EXPECT_FALSE(tor->has_all());

    tr_torrentRemove(tor, true, nullptr, nullptr);
}

} // namespace libtransmission::test
