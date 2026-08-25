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

void tr_disk_write_worker::thread_func()
{
    for (;;)
    {
        auto job = Job{};

        {
            auto lock = std::unique_lock{ mutex_ };
            cv_.wait(lock, [this]() { return stopping_ || (!paused_ && !std::empty(todo_)); });
            if (stopping_ && std::empty(todo_))
            {
                return;
            }
            job = std::move(todo_.front());
            todo_.pop_front();
            running_job_ = true;
        }

        auto const job_bytes = std::size(job.data);
        auto const err = run_job(job);

        if (job.on_done)
        {
            job.on_done(err);
        }

        {
            auto const lock = std::scoped_lock{ mutex_ };
            pending_bytes_ -= job_bytes;
            running_job_ = false;
        }
        drained_cv_.notify_all();
    }
}

void tr_disk_write_worker::set_paused(bool const paused)
{
    {
        auto const lock = std::scoped_lock{ mutex_ };
        paused_ = paused;
    }

    cv_.notify_all();
}

void tr_disk_write_worker::add(Job&& job)
{
    {
        auto const lock = std::scoped_lock{ mutex_ };

        if (stopping_)
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

        pending_bytes_ += std::size(job.data);
        todo_.emplace_back(std::move(job));

        if (!thread_.joinable())
        {
            thread_ = std::thread{ &tr_disk_write_worker::thread_func, this };
        }
    }

    cv_.notify_one();
}

size_t tr_disk_write_worker::pending_bytes() const noexcept
{
    auto const lock = std::scoped_lock{ mutex_ };
    return pending_bytes_;
}

size_t tr_disk_write_worker::pending_jobs() const noexcept
{
    auto const lock = std::scoped_lock{ mutex_ };
    return std::size(todo_) + (running_job_ ? 1U : 0U);
}

void tr_disk_write_worker::drain()
{
    TR_ASSERT(std::this_thread::get_id() != thread_.get_id());

    {
        auto const lock = std::scoped_lock{ mutex_ };
        if (paused_)
        {
            paused_ = false; // a drain outranks a test's pause; never deadlock here
        }
    }
    cv_.notify_all();

    auto lock = std::unique_lock{ mutex_ };
    drained_cv_.wait(lock, [this]() { return std::empty(todo_) && !running_job_; });
}

tr_disk_write_worker::~tr_disk_write_worker()
{
    // Everything queued still gets written. Dropping it would lose downloaded
    // data that the cache has already reported as safely handed off.
    {
        auto lock = std::unique_lock{ mutex_ };
        drained_cv_.wait(lock, [this]() { return std::empty(todo_) && !running_job_; });
        stopping_ = true;
    }

    cv_.notify_all();

    if (thread_.joinable())
    {
        thread_.join();
    }
}
