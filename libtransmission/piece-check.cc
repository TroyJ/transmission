// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include "libtransmission/transmission.h"

#include "libtransmission/crypto-utils.h"
#include "libtransmission/file.h"
#include "libtransmission/piece-check.h"

bool tr_piece_check_worker::read_spans(std::vector<Span> const& spans, std::byte* const buffer)
{
    auto pos = uint64_t{};
    auto readable = true;
    for (auto const& span : spans)
    {
        if (std::empty(span.path))
        {
            readable = false;
        }
        else if (auto const fd = tr_sys_file_open(span.path.c_str(), TR_SYS_FILE_READ, 0); fd == TR_BAD_SYS_FILE)
        {
            readable = false;
        }
        else
        {
            auto n_read = uint64_t{};
            (void)tr_sys_file_read_at(fd, buffer + pos, span.length, span.file_offset, &n_read);
            tr_sys_file_close(fd);
        }

        pos += span.length;
    }

    return readable;
}

tr_piece_check_worker::Result tr_piece_check_worker::hash_job(Job const& job, std::vector<std::byte>& buffer)
{
    buffer.assign(job.piece_size, std::byte{});

    // Read the on-disk bytes. Short reads are tolerated because the
    // missing tail may be supplied by the cache overlay below; a
    // file that cannot be opened at all is reported distinctly so
    // the caller can tell "renamed/moved under us" from "corrupt".
    auto const unreadable = !read_spans(job.spans, std::data(buffer));

    // overlay the blocks that were still in the write cache at snapshot time
    for (auto const& cached : job.cached)
    {
        if (cached.piece_offset >= job.piece_size)
        {
            continue;
        }
        auto const n = std::min(uint64_t{ std::size(cached.data) }, job.piece_size - cached.piece_offset);
        std::transform(
            std::begin(cached.data),
            std::begin(cached.data) + n,
            std::begin(buffer) + cached.piece_offset,
            [](uint8_t byte) { return std::byte{ byte }; });
    }

    if (tr_sha1::digest(buffer) == job.expected_hash)
    {
        return Result::Pass;
    }

    return unreadable ? Result::Unreadable : Result::Fail;
}

void tr_piece_check_worker::thread_func()
{
    for (;;)
    {
        auto task = std::function<void()>{};

        {
            auto lock = std::unique_lock{ mutex_ };
            cv_.wait(lock, [this]() { return stopping_ || !std::empty(todo_); });
            if (stopping_)
            {
                return;
            }
            task = std::move(todo_.front());
            todo_.pop_front();
        }

        task();
    }
}

void tr_piece_check_worker::add(Job&& job)
{
    run(
        [job = std::move(job)]()
        {
            auto buffer = std::vector<std::byte>{};
            auto const result = hash_job(job, buffer);
            if (job.on_done)
            {
                job.on_done(result);
            }
        });
}

void tr_piece_check_worker::run(std::function<void()> task)
{
    auto const lock = std::scoped_lock{ mutex_ };
    if (stopping_)
    {
        return;
    }

    todo_.emplace_back(std::move(task));

    if (!thread_.joinable())
    {
        thread_ = std::thread{ &tr_piece_check_worker::thread_func, this };
    }

    cv_.notify_one();
}

tr_piece_check_worker::~tr_piece_check_worker()
{
    {
        auto const lock = std::scoped_lock{ mutex_ };
        stopping_ = true;
        todo_.clear();
    }
    cv_.notify_all();

    if (thread_.joinable())
    {
        thread_.join();
    }
}
