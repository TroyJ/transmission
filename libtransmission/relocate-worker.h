// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "libtransmission/torrent-metainfo.h"
#include "libtransmission/transmission.h"

enum tr_torrent_relocation_state : uint8_t;

class tr_relocate_worker
{
public:
    struct Snapshot
    {
        tr_torrent_id_t torrent_id = {};
        tr_sha1_digest_t info_hash = {};
        std::string info_hash_string;
        std::string name;
        tr_torrent_metainfo metainfo;
        std::string source_root;
        std::string target_root;
        std::string previous_download_dir;
        std::string previous_incomplete_dir;
        std::string journal_file;
        bool resume_after_relocation = false;
    };

    class Mediator
    {
    public:
        virtual ~Mediator() = default;

        [[nodiscard]] virtual Snapshot const& snapshot() const = 0;

        virtual void on_relocate_state_changed(
            tr_torrent_relocation_state state,
            uint64_t bytes_copied,
            uint64_t bytes_total,
            uint64_t rate_bps,
            std::string_view error) = 0;

        [[nodiscard]] virtual bool on_verified_location_ready() = 0;
        virtual void on_source_deleted() = 0;
    };

    tr_relocate_worker() = default;
    ~tr_relocate_worker();

    tr_relocate_worker(tr_relocate_worker const&) = delete;
    tr_relocate_worker(tr_relocate_worker&&) = delete;
    tr_relocate_worker& operator=(tr_relocate_worker const&) = delete;
    tr_relocate_worker& operator=(tr_relocate_worker&&) = delete;

    [[nodiscard]] bool add(std::unique_ptr<Mediator> mediator, tr_priority_t priority);
    // Stop relocating `info_hash` and forget it. Never waits: if the relocate
    // thread is on that torrent it is asked to stop and `on_stopped` runs on
    // the relocate thread once it has, so a caller can safely delete the staged
    // files from there. Otherwise `on_stopped` runs inline.
    void remove(tr_sha1_digest_t const& info_hash, std::function<void()> on_stopped = {});
    bool cancel(tr_sha1_digest_t const& info_hash);
    void prepare_shutdown();

    // Delete the staged `.trreloc.<hash>.tmp` copies and the journal for a
    // relocation that will never be resumed (e.g. the torrent is being removed).
    // Must not be called while the relocation is queued or running.
    static void discard_staged_files(Snapshot const& snapshot);

    // For journals whose torrent no longer exists we have no metainfo, so walk
    // `root` and delete every `*.trreloc.<info_hash_string>.tmp` under it.
    // Returns the number of files removed. Safe to call from any thread.
    static size_t discard_orphaned_temp_files(std::string_view root, std::string_view info_hash_string);

    // The little the sweep needs from a journal whose torrent is gone.
    struct JournalRoots
    {
        std::string target_root;
        std::string name; // empty for journals written before `name` was recorded
    };
    [[nodiscard]] static std::optional<JournalRoots> read_journal_roots(std::string_view journal_file);

private:
    struct Node
    {
        Node(std::unique_ptr<Mediator> mediator, tr_priority_t priority) noexcept
            : mediator_{ std::move(mediator) }
            , priority_{ priority }
        {
        }

        [[nodiscard]] int compare(Node const& that) const noexcept;

        [[nodiscard]] auto operator<(Node const& that) const noexcept
        {
            return compare(that) < 0;
        }

        [[nodiscard]] bool matches(tr_sha1_digest_t const& info_hash) const noexcept
        {
            return mediator_->snapshot().info_hash == info_hash;
        }

        std::unique_ptr<Mediator> mediator_;
        tr_priority_t priority_;
    };

    void relocate_thread_func();

    std::mutex relocate_mutex_;
    std::set<Node> todo_;
    std::optional<Node> current_node_;
    std::optional<std::thread::id> relocate_thread_id_;
    std::atomic<bool> stop_current_ = false;
    std::atomic<bool> cancel_current_ = false;
    bool shutdown_requested_ = false;
    std::vector<std::function<void()>> stopped_callbacks_;
    std::condition_variable stop_current_cv_;
    std::condition_variable state_cv_;
};
