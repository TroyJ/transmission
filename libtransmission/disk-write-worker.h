// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "libtransmission/transmission.h"

#include "libtransmission/file.h" // tr_sys_file_t

/**
 * Performs cache flushes on a background thread so that the session mutex is
 * never held across a blocking write.
 *
 * A slow disk is not a hypothetical: on an exFAT/FSKit USB SSD under load, a
 * single 256 KiB `pwrite` has been measured taking 134 seconds. Done inline on
 * the session thread, as it was, that froze the GUI and the RPC server for the
 * duration. See docs/async-write-path-2a.md.
 *
 * A job is a self-contained snapshot. The caller resolves and opens the files
 * while holding the session lock -- opens are microseconds, writes are not --
 * and hands over **duplicated** descriptors plus a private copy of the bytes.
 * The worker therefore never touches tr_session, tr_torrent, or the shared
 * `tr_open_files` pool, and cannot be tripped up by the fd pool evicting a
 * descriptor mid-write. It owns the descriptors it is given and closes them.
 *
 * Ordering: a single FIFO thread. Two flushes covering the same file region
 * must not reorder, and one thread makes that true by construction. The disk
 * is the bottleneck regardless, so there is nothing to gain from concurrency.
 */
class tr_disk_write_worker
{
public:
    /** One contiguous run of bytes destined for one file. */
    struct Chunk
    {
        tr_sys_file_t fd = TR_BAD_SYS_FILE; // owned by the job; closed when it completes
        uint64_t file_offset = 0U;
        uint64_t length = 0U;
    };

    struct Job
    {
        std::vector<Chunk> chunks; // in order; lengths sum to size(data)
        std::vector<uint8_t> data; // a private copy, so the cache may drop its blocks
        // Invoked on the worker thread with 0 on success or an errno. The
        // callback is responsible for re-entering the session thread before
        // touching any session state.
        std::function<void(int)> on_done;
    };

    tr_disk_write_worker() = default;
    ~tr_disk_write_worker();

    tr_disk_write_worker(tr_disk_write_worker const&) = delete;
    tr_disk_write_worker(tr_disk_write_worker&&) = delete;
    tr_disk_write_worker& operator=(tr_disk_write_worker const&) = delete;
    tr_disk_write_worker& operator=(tr_disk_write_worker&&) = delete;

    void add(Job&& job);

    /** Bytes accepted but not yet written. The backpressure signal. */
    [[nodiscard]] size_t pending_bytes() const noexcept;

    [[nodiscard]] size_t pending_jobs() const noexcept;

    /**
     * Block until every queued job has been written and its callback has run.
     *
     * Required before the cache or session is torn down: unlike the old
     * synchronous flush, an async one makes quit-safety explicit rather than
     * implicit. Must not be called from the worker thread.
     */
    void drain();

    /** Runs the job's writes. Exposed for testing. Closes the job's fds. */
    [[nodiscard]] static int run_job(Job& job);

    /**
     * Holds queued jobs without writing them. Testing only.
     *
     * Lets a test park a write in flight and assert that the block is still
     * readable while it is there -- the property that makes the asynchronous
     * write safe, and one that is otherwise a race to observe.
     */
    void set_paused(bool paused);

private:
    void thread_func();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable drained_cv_;
    std::deque<Job> todo_;
    std::thread thread_;
    size_t pending_bytes_ = 0U;
    bool running_job_ = false;
    bool paused_ = false;
    bool stopping_ = false;
};
