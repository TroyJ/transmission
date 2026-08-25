// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include "libtransmission/transmission.h"

#include "libtransmission/disk-write-worker.h"
#include "libtransmission/error.h"
#include "libtransmission/file.h"
#include "libtransmission/open-files.h"
#include "libtransmission/tr-assert.h"

namespace
{

void close_chunks(std::vector<tr_disk_write_worker::Chunk>& chunks) noexcept
{
    for (auto& chunk : chunks)
    {
        if (chunk.fd != TR_BAD_SYS_FILE)
        {
            tr_sys_file_close(chunk.fd);
            chunk.fd = TR_BAD_SYS_FILE;
        }
    }
}

} // namespace

int tr_disk_write_worker::run_job(Job& job)
{
    auto err = int{};
    auto const* walk = std::data(job.data);
    auto remaining = std::size(job.data);

    for (auto const& chunk : job.chunks)
    {
        if (err != 0)
        {
            break;
        }

        if (chunk.fd == TR_BAD_SYS_FILE && !std::empty(chunk.path))
        {
            auto error = tr_error{};
            auto& mutable_chunk = const_cast<Chunk&>(chunk); // the job owns its chunks; this records the fd for close_chunks()
            mutable_chunk.fd = tr_open_files::open_file(
                chunk.path,
                true,
                static_cast<tr_open_files::Preallocation>(chunk.preallocation),
                chunk.file_size,
                &error);
            if (mutable_chunk.fd == TR_BAD_SYS_FILE)
            {
                err = error.code() != 0 ? error.code() : EIO;
                break;
            }
        }

        if (chunk.fd == TR_BAD_SYS_FILE)
        {
            err = EBADF;
            break;
        }

        auto len = chunk.length;
        if (len > remaining)
        {
            // The snapshot and the buffer disagree; refuse rather than write garbage.
            err = EINVAL;
            break;
        }

        auto offset = chunk.file_offset;
        while (len > 0U && err == 0)
        {
            auto n_written = uint64_t{};
            auto error = tr_error{};

            if (!tr_sys_file_write_at(chunk.fd, walk, len, offset, &n_written, &error))
            {
                err = error.code() != 0 ? error.code() : EIO;
                break;
            }

            if (n_written == 0U)
            {
                err = EIO; // no progress; do not spin
                break;
            }

            walk += n_written;
            offset += n_written;
            len -= n_written;
            remaining -= n_written;
        }
    }

    close_chunks(job.chunks);
    return err;
}

void tr_disk_write_worker::thread_func(std::shared_ptr<State> const state)
{
    auto& st = *state;

    for (;;)
    {
        auto job = Job{};

        {
            auto lock = std::unique_lock{ st.mutex };
            st.cv.wait(lock, [&st]() { return st.stopping || (!st.paused && !std::empty(st.todo)); });
            if (st.stopping && std::empty(st.todo))
            {
                return;
            }
            job = std::move(st.todo.front());
            st.todo.pop_front();
            st.running_job = true;
        }

        auto const job_bytes = std::size(job.data);
        auto const err = run_job(job);

        if (job.on_done)
        {
            job.on_done(err);
        }

        {
            auto const lock = std::scoped_lock{ st.mutex };
            st.pending_bytes -= job_bytes;
            st.running_job = false;
        }
        st.drained_cv.notify_all();
    }
}

void tr_disk_write_worker::set_paused(bool const paused)
{
    {
        auto const lock = std::scoped_lock{ state_->mutex };
        state_->paused = paused;
    }

    state_->cv.notify_all();
}

void tr_disk_write_worker::add(Job&& job)
{
    auto& st = *state_;

    {
        auto const lock = std::scoped_lock{ st.mutex };

        if (st.abandoned)
        {
            // Shutdown gave up on the disk. The caller has already recorded
            // that these bytes never landed; the descriptors are deliberately
            // not closed, since close() is what may be stuck.
            return;
        }

        if (st.stopping)
        {
            // Shutting down: do not silently drop the data on the floor.
            // Write it here, on the caller's thread, so nothing is lost.
            auto local = std::move(job);
            auto const err = run_job(local);
            if (local.on_done)
            {
                local.on_done(err);
            }
            return;
        }

        st.pending_bytes += std::size(job.data);
        st.todo.emplace_back(std::move(job));

        if (!thread_.joinable())
        {
            thread_ = std::thread{ &tr_disk_write_worker::thread_func, state_ };
        }
    }

    st.cv.notify_one();
}

size_t tr_disk_write_worker::pending_bytes() const noexcept
{
    auto const lock = std::scoped_lock{ state_->mutex };
    return state_->pending_bytes;
}

size_t tr_disk_write_worker::pending_jobs() const noexcept
{
    auto const lock = std::scoped_lock{ state_->mutex };
    return std::size(state_->todo) + (state_->running_job ? 1U : 0U);
}

bool tr_disk_write_worker::is_abandoned() const noexcept
{
    auto const lock = std::scoped_lock{ state_->mutex };
    return state_->abandoned;
}

void tr_disk_write_worker::drain()
{
    {
        auto const lock = std::scoped_lock{ state_->mutex };
        if (state_->paused)
        {
            state_->paused = false; // an unbounded drain outranks a test's pause; never deadlock here
        }
    }
    state_->cv.notify_all();

    static_cast<void>(drain_for(std::chrono::milliseconds::max()));
}

bool tr_disk_write_worker::drain_for(std::chrono::milliseconds const timeout)
{
    // Deliberately does not un-pause: a bounded wait cannot deadlock, and a
    // paused worker is how tests model a disk that never answers.
    TR_ASSERT(std::this_thread::get_id() != thread_.get_id());
    auto& st = *state_;

    auto lock = std::unique_lock{ st.mutex };
    auto const done = [&st]()
    {
        return std::empty(st.todo) && !st.running_job;
    };
    if (timeout == std::chrono::milliseconds::max())
    {
        st.drained_cv.wait(lock, done);
        return true;
    }
    return st.drained_cv.wait_for(lock, timeout, done);
}

size_t tr_disk_write_worker::abandon()
{
    auto& st = *state_;
    auto n_dropped = size_t{};

    {
        auto const lock = std::scoped_lock{ st.mutex };
        n_dropped = std::size(st.todo);
        st.todo.clear();
        st.pending_bytes = 0U;
        st.abandoned = true;
        st.stopping = true;
    }
    st.cv.notify_all();

    // A thread mid-syscall cannot be joined. It holds the state by
    // shared_ptr, so letting it go is safe; it will finish its one write
    // (or not -- the process is exiting) and return.
    if (thread_.joinable())
    {
        thread_.detach();
    }

    return n_dropped;
}

tr_disk_write_worker::~tr_disk_write_worker()
{
    auto& st = *state_;

    // Everything queued still gets written. Dropping it would lose downloaded
    // data that the cache has already reported as safely handed off.
    {
        auto lock = std::unique_lock{ st.mutex };
        if (!st.abandoned)
        {
            st.drained_cv.wait(lock, [&st]() { return std::empty(st.todo) && !st.running_job; });
        }
        st.stopping = true;
    }

    st.cv.notify_all();

    if (thread_.joinable())
    {
        thread_.join();
    }
}
