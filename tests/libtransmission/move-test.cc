// This file Copyright (C) 2013-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <libtransmission/transmission.h>

#include <libtransmission/block-info.h>
#include <libtransmission/cache.h> // tr_cacheWriteBlock()
#include <libtransmission/file.h> // tr_sys_path_*()
#include <libtransmission/io-trace.h>
#include <libtransmission/quark.h>
#include <libtransmission/relocate-worker.h>
#include <libtransmission/torrent.h>
#include <libtransmission/torrent-files.h>
#include <libtransmission/tr-strbuf.h>
#include <libtransmission/variant.h>

#include "gtest/gtest.h"
#include "test-fixtures.h"

using namespace std::literals;

namespace libtransmission::test
{

auto constexpr MaxWaitMsec = 5000;

[[nodiscard]] bool waitForRelocationToFinish(tr_torrent* const tor, size_t const max_wait_msec)
{
    auto const done = [tor]()
    {
        auto const state = tr_torrentStat(tor)->relocationState;
        return state == TR_RELOC_NONE || state == TR_RELOC_ERROR || state == TR_RELOC_CANCELLED;
    };

    if (!waitFor(done, max_wait_msec))
    {
        return false;
    }

    auto const state = tr_torrentStat(tor)->relocationState;
    return state != TR_RELOC_ERROR && state != TR_RELOC_CANCELLED;
}

[[nodiscard]] bool waitForRelocationState(
    tr_torrent* const tor,
    tr_torrent_relocation_state const expected_state,
    size_t const max_wait_msec)
{
    return waitFor([tor, expected_state]() { return tr_torrentStat(tor)->relocationState == expected_state; }, max_wait_msec);
}

class IncompleteDirTest
    : public SessionTest
    , public ::testing::WithParamInterface<std::pair<std::string, std::string>>
{
protected:
    void SetUp() override
    {
        if (auto* map = settings()->get_if<tr_variant::Map>(); map != nullptr)
        {
            auto const download_dir = GetParam().second;
            map->insert_or_assign(TR_KEY_download_dir, download_dir);
            auto const incomplete_dir = GetParam().first;
            map->insert_or_assign(TR_KEY_incomplete_dir, incomplete_dir);
            map->insert_or_assign(TR_KEY_incomplete_dir_enabled, true);
        }

        SessionTest::SetUp();
    }

    static auto constexpr MaxWaitMsec = 3000;
};

TEST_P(IncompleteDirTest, incompleteDir)
{
    auto const* download_dir = tr_sessionGetDownloadDir(session_);
    auto const* incomplete_dir = tr_sessionGetIncompleteDir(session_);

    // init an incomplete torrent.
    // the test zero_torrent will be missing its first piece.
    tr_sessionSetIncompleteFileNamingEnabled(session_, true);
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Partial);
    auto path = tr_pathbuf{};

    path.assign(incomplete_dir, '/', tr_torrentFile(tor, 0).name, tr_torrent_files::PartialFileSuffix);
    EXPECT_EQ(path, tr_torrentFindFile(tor, 0));
    path.assign(incomplete_dir, '/', tr_torrentFile(tor, 1).name);
    EXPECT_EQ(path, tr_torrentFindFile(tor, 1));
    EXPECT_EQ(tor->piece_size(), tr_torrentStat(tor)->leftUntilDone);

    auto completeness = TR_LEECH;
    auto const zeroes_completeness_func =
        [](tr_torrent* /*torrent*/, tr_completeness c, bool /*was_running*/, void* vc) noexcept
    {
        *static_cast<tr_completeness*>(vc) = c;
    };
    tr_sessionSetCompletenessCallback(session_, zeroes_completeness_func, &completeness);

    struct TestIncompleteDirData
    {
        tr_session* session = {};
        tr_torrent* tor = {};
        tr_block_index_t block = {};
        tr_piece_index_t pieceIndex = {};
        std::unique_ptr<Cache::BlockData> buf;
        bool done = {};
    };

    auto const test_incomplete_dir_threadfunc = [](TestIncompleteDirData* data) noexcept
    {
        data->session->cache->write_block(data->tor->id(), data->block, std::move(data->buf));
        data->tor->on_block_received(data->block);
        data->done = true;
    };

    // now finish writing it
    {
        auto data = TestIncompleteDirData{};
        data.session = session_;
        data.tor = tor;

        auto const [begin, end] = tor->block_span_for_piece(data.pieceIndex);

        for (tr_block_index_t block_index = begin; block_index < end; ++block_index)
        {
            data.buf = std::make_unique<Cache::BlockData>(tr_block_info::BlockSize);
            std::fill_n(std::data(*data.buf), tr_block_info::BlockSize, '\0');
            data.block = block_index;
            data.done = false;
            session_->run_in_session_thread(test_incomplete_dir_threadfunc, &data);

            auto const test = [&data]()
            {
                return data.done;
            };
            EXPECT_TRUE(waitFor(test, MaxWaitMsec));
        }
    }

    blockingTorrentVerify(tor);
    EXPECT_EQ(0, tr_torrentStat(tor)->leftUntilDone);

    auto test = [&completeness]()
    {
        return completeness != TR_LEECH;
    };
    EXPECT_TRUE(waitFor(test, MaxWaitMsec));
    EXPECT_EQ(TR_SEED, completeness);

    // The move out of the incomplete dir is queued behind the torrent's last
    // writes rather than started synchronously with the completeness change,
    // so wait for it to begin as well as to finish.
    auto const moved_to_download_dir = [tor, &download_dir]()
    {
        return tor->current_dir() == download_dir;
    };
    EXPECT_TRUE(waitFor(moved_to_download_dir, MaxWaitMsec));
    EXPECT_TRUE(waitForRelocationToFinish(tor, MaxWaitMsec));

    auto const n = tr_torrentFileCount(tor);
    for (tr_file_index_t i = 0; i < n; ++i)
    {
        auto const expected = tr_pathbuf{ download_dir, '/', tr_torrentFile(tor, i).name };
        EXPECT_EQ(expected, tr_torrentFindFile(tor, i));
    }

    // cleanup
    tr_torrentRemove(tor, true, nullptr, nullptr);
}

INSTANTIATE_TEST_SUITE_P(
    IncompleteDir,
    IncompleteDirTest,
    ::testing::Values(
        // what happens when incompleteDir is a subdir of downloadDir
        std::make_pair(std::string{ "Downloads/Incomplete" }, std::string{ "Downloads" }),
        // test what happens when downloadDir is a subdir of incompleteDir
        std::make_pair(std::string{ "Downloads" }, std::string{ "Downloads/Complete" }),
        // test what happens when downloadDir and incompleteDir are siblings
        std::make_pair(std::string{ "Incomplete" }, std::string{ "Downloads" })));

/***
****
***/

using MoveTest = SessionTest;

TEST_F(MoveTest, setLocation)
{
    auto const target_dir = tr_pathbuf{ session_->configDir(), "/target"sv };
    tr_sys_dir_create(target_dir.data(), TR_SYS_DIR_CREATE_PARENTS, 0777, nullptr);

    // init a torrent.
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    blockingTorrentVerify(tor);
    EXPECT_EQ(0, tr_torrentStat(tor)->leftUntilDone);

    // now move it
    auto state = -1;
    tr_torrentSetLocation(tor, target_dir, true, &state);
    auto test = [&state]()
    {
        return state == TR_LOC_DONE;
    };
    EXPECT_TRUE(waitFor(test, MaxWaitMsec));
    EXPECT_EQ(TR_LOC_DONE, state);
    EXPECT_TRUE(waitForRelocationToFinish(tor, MaxWaitMsec));

    // confirm the torrent is still complete after being moved
    blockingTorrentVerify(tor);
    EXPECT_EQ(0, tr_torrentStat(tor)->leftUntilDone);

    // confirm the files really got moved
    sync();
    auto const n = tr_torrentFileCount(tor);
    for (tr_file_index_t i = 0; i < n; ++i)
    {
        auto const expected = tr_pathbuf{ target_dir, '/', tr_torrentFile(tor, i).name };
        EXPECT_EQ(expected, tr_torrentFindFile(tor, i));
    }

    // cleanup
    tr_torrentRemove(tor, true, nullptr, nullptr);
}

TEST_F(MoveTest, setLocationWithoutLocalDataUpdatesDownloadDirWithoutRelocation)
{
    auto const target_dir = tr_pathbuf{ session_->configDir(), "/target-no-data"sv };
    tr_sys_dir_create(target_dir.data(), TR_SYS_DIR_CREATE_PARENTS, 0777, nullptr);

    auto* const tor = zeroTorrentInit(ZeroTorrentState::NoFiles);
    ASSERT_NE(nullptr, tor);
    EXPECT_FALSE(tor->has_any_local_data());
    EXPECT_NE(std::string_view{ target_dir }, tor->download_dir().sv());

    auto state = -1;
    tr_torrentSetLocation(tor, target_dir, true, &state);
    ASSERT_TRUE(waitFor([&state]() { return state == TR_LOC_DONE; }, MaxWaitMsec));

    EXPECT_EQ(TR_LOC_DONE, state);
    EXPECT_EQ(TR_RELOC_NONE, tr_torrentStat(tor)->relocationState);
    EXPECT_EQ(std::string_view{ target_dir }, tor->download_dir().sv());
    EXPECT_TRUE(std::empty(tr_torrentFindFile(tor, 0)));

    tr_torrentRemove(tor, false, nullptr, nullptr);
}

TEST_F(MoveTest, setLocationWithMoveProbesForLocalDataOffTheSessionLock)
{
    auto const target_dir = tr_pathbuf{ session_->configDir(), "/target-probe"sv };
    tr_sys_dir_create(target_dir.data(), TR_SYS_DIR_CREATE_PARENTS, 0777, nullptr);

    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    blockingTorrentVerify(tor);
    tor->forget_found_paths(); // so the probe has to stat()

    // The RPC handler calls set_location() under the session lock; the
    // has-any-local-data probe must not stat() the source volume there.
    auto const path_ops_under_lock_before = tr_io_trace::locked_count(tr_io_trace::Op::Path);
    auto state = -1;
    {
        auto const lock = session_->unique_lock();
        tr_torrentSetLocation(tor, target_dir, true, &state);
    }
    EXPECT_TRUE(waitFor([&state]() { return state == TR_LOC_DONE; }, MaxWaitMsec));
    EXPECT_TRUE(waitForRelocationToFinish(tor, MaxWaitMsec));
    EXPECT_EQ(path_ops_under_lock_before, tr_io_trace::locked_count(tr_io_trace::Op::Path));

    EXPECT_EQ(std::string_view{ target_dir }, tor->download_dir().sv());
    blockingTorrentVerify(tor);
    EXPECT_EQ(0, tr_torrentStat(tor)->leftUntilDone);

    tr_torrentRemove(tor, true, nullptr, nullptr);
}

TEST_F(MoveTest, relocationControlPredicates)
{
    auto* const tor = zeroTorrentInit(ZeroTorrentState::NoFiles);
    ASSERT_NE(nullptr, tor);

    tor->set_relocation_state(TR_RELOC_ERROR, 0U, 0U, 0U, {});
    EXPECT_TRUE(tr_torrentCanRetryRelocation(tor));
    EXPECT_FALSE(tr_torrentCanResumeRelocation(tor));
    EXPECT_FALSE(tr_torrentCanCancelRelocation(tor));

    tor->set_relocation_state(TR_RELOC_COPYING, 1U, 2U, 0U, {});
    EXPECT_FALSE(tr_torrentCanRetryRelocation(tor));
    EXPECT_FALSE(tr_torrentCanResumeRelocation(tor));
    EXPECT_TRUE(tr_torrentCanCancelRelocation(tor));

    // a cancel that has been asked for but not yet noticed by the relocate thread
    tor->set_relocation_state(TR_RELOC_CANCELLING, 1U, 2U, 0U, {});
    EXPECT_FALSE(tr_torrentCanRetryRelocation(tor));
    EXPECT_FALSE(tr_torrentCanResumeRelocation(tor));
    EXPECT_FALSE(tr_torrentCanCancelRelocation(tor)); // no second cancel while one is pending

    tor->set_relocation_state(TR_RELOC_CANCELLED, 1U, 2U, 0U, {});
    EXPECT_FALSE(tr_torrentCanRetryRelocation(tor));
    EXPECT_TRUE(tr_torrentCanResumeRelocation(tor));
    EXPECT_FALSE(tr_torrentCanCancelRelocation(tor));

    tr_torrentRemove(tor, false, nullptr, nullptr);
}

TEST_F(MoveTest, failedRelocationRestartsRunningTorrent)
{
    auto const invalid_target = tr_pathbuf{ session_->configDir(), "/invalid-target"sv };
    auto const bad_fd = tr_sys_file_open(invalid_target, TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE | TR_SYS_FILE_TRUNCATE, 0600);
    ASSERT_NE(TR_BAD_SYS_FILE, bad_fd);
    ASSERT_TRUE(tr_sys_file_close(bad_fd, nullptr));

    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    ASSERT_NE(nullptr, tor);
    blockingTorrentVerify(tor);
    EXPECT_EQ(0, tr_torrentStat(tor)->leftUntilDone);

    tr_torrentStartNow(tor);
    ASSERT_TRUE(waitFor([tor]() { return tor->is_running(); }, MaxWaitMsec));

    auto state = -1;
    tr_torrentSetLocation(tor, invalid_target, true, &state);

    ASSERT_TRUE(waitFor([&state]() { return state == TR_LOC_ERROR; }, MaxWaitMsec));
    ASSERT_TRUE(waitForRelocationState(tor, TR_RELOC_ERROR, MaxWaitMsec));
    EXPECT_TRUE(waitFor([tor]() { return tor->is_running(); }, MaxWaitMsec));

    tr_torrentRemove(tor, false, nullptr, nullptr);
}

TEST_F(MoveTest, failedRelocationKeepsPausedTorrentStopped)
{
    auto const invalid_target = tr_pathbuf{ session_->configDir(), "/invalid-target"sv };
    auto const bad_fd = tr_sys_file_open(invalid_target, TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE | TR_SYS_FILE_TRUNCATE, 0600);
    ASSERT_NE(TR_BAD_SYS_FILE, bad_fd);
    ASSERT_TRUE(tr_sys_file_close(bad_fd, nullptr));

    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    ASSERT_NE(nullptr, tor);
    blockingTorrentVerify(tor);
    EXPECT_EQ(0, tr_torrentStat(tor)->leftUntilDone);
    ASSERT_FALSE(tor->is_running());

    auto state = -1;
    tr_torrentSetLocation(tor, invalid_target, true, &state);

    ASSERT_TRUE(waitFor([&state]() { return state == TR_LOC_ERROR; }, MaxWaitMsec));
    ASSERT_TRUE(waitForRelocationState(tor, TR_RELOC_ERROR, MaxWaitMsec));
    EXPECT_FALSE(tor->is_running());

    tr_torrentRemove(tor, false, nullptr, nullptr);
}

// A mediator whose first "copying" notification blocks, standing in for the
// relocate thread being stuck inside a slow chunk on a stalled volume. The
// worker owns and destroys the mediator, so what the test observes lives in a
// shared state object that outlives it.
struct BlockedRelocateState
{
    std::atomic<bool> copying = false;
    std::atomic<bool> cancelled = false;
    std::atomic<tr_torrent_relocation_state> last_state = TR_RELOC_NONE;
    std::atomic<bool> mediator_destroyed = false; // the relocate thread is done with the torrent
    std::mutex mutex;
    std::condition_variable cv;
    bool released = false;

    void release()
    {
        {
            auto const lock = std::scoped_lock{ mutex };
            released = true;
        }
        cv.notify_all();
    }
};

class BlockingRelocateMediator final : public tr_relocate_worker::Mediator
{
public:
    BlockingRelocateMediator(tr_relocate_worker::Snapshot snapshot, std::shared_ptr<BlockedRelocateState> state)
        : snapshot_{ std::move(snapshot) }
        , state_{ std::move(state) }
    {
    }

    ~BlockingRelocateMediator() override
    {
        state_->mediator_destroyed.store(true);
    }

    [[nodiscard]] tr_relocate_worker::Snapshot const& snapshot() const override
    {
        return snapshot_;
    }

    void on_relocate_state_changed(
        tr_torrent_relocation_state const state,
        uint64_t /*bytes_copied*/,
        uint64_t /*bytes_total*/,
        uint64_t /*rate_bps*/,
        std::string_view /*error*/) override
    {
        state_->last_state.store(state);

        if (state == TR_RELOC_CANCELLED)
        {
            state_->cancelled.store(true);
            return;
        }

        if (state != TR_RELOC_COPYING || state_->copying.exchange(true))
        {
            return;
        }

        auto lock = std::unique_lock{ state_->mutex };
        state_->cv.wait_for(lock, std::chrono::seconds{ 5 }, [this]() { return state_->released; });
    }

    [[nodiscard]] bool on_verified_location_ready() override
    {
        return false;
    }

    void on_source_deleted() override
    {
    }

private:
    tr_relocate_worker::Snapshot snapshot_;
    std::shared_ptr<BlockedRelocateState> state_;
};

class RelocateWorkerTest : public SessionTest
{
protected:
    [[nodiscard]] tr_relocate_worker::Snapshot makeSnapshot(tr_torrent const* tor, std::string_view target_root) const
    {
        auto snapshot = tr_relocate_worker::Snapshot{};
        snapshot.torrent_id = tor->id();
        snapshot.info_hash = tor->info_hash();
        snapshot.info_hash_string = std::string{ tor->info_hash_string() };
        snapshot.name = std::string{ tor->name() };
        snapshot.metainfo = tor->metainfo();
        snapshot.source_root = std::string{ tor->current_dir() };
        snapshot.target_root = std::string{ target_root };
        snapshot.previous_download_dir = std::string{ tor->download_dir() };
        snapshot.journal_file = tor->relocation_journal_file();
        return snapshot;
    }

    // Parks the relocate thread inside its first "copying" notification.
    [[nodiscard]] std::shared_ptr<BlockedRelocateState> startBlockedRelocation(
        tr_relocate_worker& worker,
        tr_torrent const* tor,
        std::string_view target_dir)
    {
        auto state = std::make_shared<BlockedRelocateState>();
        auto mediator = std::make_unique<BlockingRelocateMediator>(makeSnapshot(tor, target_dir), state);
        if (!worker.add(std::move(mediator), TR_PRI_NORMAL) ||
            !waitFor([&state]() { return state->copying.load(); }, MaxWaitMsec))
        {
            return {};
        }

        return state;
    }

    static auto constexpr PromptMsec = 500; // a call that does not wait has no excuse for taking this long
};

TEST_F(RelocateWorkerTest, cancelDoesNotWaitForTheCurrentChunk)
{
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    ASSERT_NE(nullptr, tor);
    auto const target_dir = tr_pathbuf{ session_->configDir(), "/cancel-target"sv };
    tr_sys_dir_create(target_dir.data(), TR_SYS_DIR_CREATE_PARENTS, 0777, nullptr);

    auto worker = tr_relocate_worker{};
    auto const state = startBlockedRelocation(worker, tor, target_dir.sv());
    ASSERT_NE(nullptr, state);

    // the relocate thread is stuck mid-copy; cancelling must not join it
    auto const began = std::chrono::steady_clock::now();
    EXPECT_TRUE(worker.cancel(tor->info_hash()));
    auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
    EXPECT_LT(elapsed.count(), PromptMsec);

    // ...and it says so straight away, rather than looking ignored
    EXPECT_EQ(TR_RELOC_CANCELLING, state->last_state.load());

    state->release();
    EXPECT_TRUE(waitFor([&state]() { return state->cancelled.load(); }, MaxWaitMsec));

    tr_torrentRemove(tor, false, nullptr, nullptr);
}

TEST_F(RelocateWorkerTest, removeDoesNotWaitButStillCleansUpAfterTheCopyStops)
{
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    ASSERT_NE(nullptr, tor);
    auto const target_dir = tr_pathbuf{ session_->configDir(), "/remove-target"sv };
    tr_sys_dir_create(target_dir.data(), TR_SYS_DIR_CREATE_PARENTS, 0777, nullptr);

    auto worker = tr_relocate_worker{};
    auto const state = startBlockedRelocation(worker, tor, target_dir.sv());
    ASSERT_NE(nullptr, state);

    auto cleaned_up = std::make_shared<std::atomic<bool>>(false);
    auto const began = std::chrono::steady_clock::now();
    worker.remove(tor->info_hash(), [cleaned_up]() { cleaned_up->store(true); });
    auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
    EXPECT_LT(elapsed.count(), PromptMsec);

    // ...and the staged files are not touched while the copy is still running
    EXPECT_FALSE(cleaned_up->load());

    state->release();
    EXPECT_TRUE(waitFor([&cleaned_up]() { return cleaned_up->load(); }, MaxWaitMsec));

    tr_torrentRemove(tor, false, nullptr, nullptr);
}

TEST_F(RelocateWorkerTest, shutdownGivesUpOnAParkedCopyAndTheDestructorDoesNotWait)
{
    // What quit does when the relocate thread is inside a chunk the volume
    // never finishes: ask, wait a bounded time, then let the thread go.
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    ASSERT_NE(nullptr, tor);
    auto const target_dir = tr_pathbuf{ session_->configDir(), "/shutdown-target"sv };
    tr_sys_dir_create(target_dir.data(), TR_SYS_DIR_CREATE_PARENTS, 0777, nullptr);

    auto worker = std::make_unique<tr_relocate_worker>();
    auto const state = startBlockedRelocation(*worker, tor, target_dir.sv());
    ASSERT_NE(nullptr, state);

    auto const began = std::chrono::steady_clock::now();
    worker->prepare_shutdown(); // asks; never waits
    EXPECT_FALSE(worker->wait_for_idle(std::chrono::milliseconds{ 100 })); // a bounded wait on a stuck thread gives up
    worker->abandon();
    EXPECT_TRUE(worker->is_abandoned());
    worker.reset(); // ...and the destructor no longer joins it
    auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
    EXPECT_LT(elapsed.count(), PromptMsec);
    EXPECT_FALSE(state->mediator_destroyed.load()) << "the relocate thread was still parked";

    // the thread owns its state jointly, so it finishes cleanly once the volume answers
    state->release();
    EXPECT_TRUE(waitFor([&state]() { return state->mediator_destroyed.load(); }, MaxWaitMsec));

    // ...and the journal is still there for the next start to resume from
    EXPECT_TRUE(tr_sys_path_exists(tor->relocation_journal_file()));

    tr_torrentRemove(tor, false, nullptr, nullptr);
}

TEST_F(MoveTest, quitWithARelocationStuckOnAStalledVolumeIsBounded)
{
    // Live, this was a quit that hung past the restart script's 30 s and ended
    // in a SIGKILL (docs/gui-clickthrough-validation.md N2). Every write to the
    // target volume stalls for longer than the shutdown grace, so the relocate
    // thread is inside one when the session closes.
    auto* const tor = zeroTorrentInit(ZeroTorrentState::Complete);
    ASSERT_NE(nullptr, tor);
    auto const info_hash = tor->info_hash();
    auto const journal_file = tor->relocation_journal_file();
    auto const target_dir = tr_pathbuf{ sandboxDir(), "/stalled-target"sv };
    tr_sys_dir_create(target_dir.data(), TR_SYS_DIR_CREATE_PARENTS, 0777, nullptr);

    static auto constexpr StallPerWrite = tr_session::ShutdownDiskGrace + std::chrono::seconds{ 4 };
    tr_io_trace::set_injected_delay(StallPerWrite, 1U << static_cast<unsigned>(tr_io_trace::Op::Write), target_dir.sv());

    tr_torrentSetLocation(tor, target_dir.c_str(), true, nullptr);
    ASSERT_TRUE(waitForRelocationState(tor, TR_RELOC_COPYING, MaxWaitMsec));
    std::this_thread::sleep_for(std::chrono::milliseconds{ 250 }); // let the first write begin its stall

    auto const began = std::chrono::steady_clock::now();
    tr_sessionClose(session_);
    auto const took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began);
    tr_io_trace::set_injected_delay(std::chrono::milliseconds{ 0 });

    EXPECT_LT(took, tr_session::ShutdownDiskGrace + std::chrono::seconds{ 3 })
        << "shutdown waited on the relocate thread past the grace";
    EXPECT_TRUE(tr_sys_path_exists(journal_file)) << "the journal must survive so the relocation resumes next start";

    // ...and it does: the next session picks the relocation back up
    session_ = tr_sessionInit(sandboxDir(), true, *settings()); // TearDown closes whatever is here
    auto* const ctor = tr_ctorNew(session_);
    tr_sessionLoadTorrents(session_, ctor);
    tr_ctorFree(ctor);
    auto* const reloaded = session_->torrents().get(info_hash);
    ASSERT_NE(nullptr, reloaded);
    EXPECT_TRUE(waitFor([reloaded]() { return reloaded->relocation_state() != TR_RELOC_NONE; }, MaxWaitMsec));
}

} // namespace libtransmission::test
