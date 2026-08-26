// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include "libtransmission/transmission.h"

#include "libtransmission/io-trace.h"
#include "libtransmission/session-lock.h"

namespace
{

// The session mutex is recursive, so a thread can already hold it when it takes
// it again. Only the outermost hold is timed; to everyone blocked behind the
// mutex, a nested hold is just more of the same wait.
thread_local auto depth = std::size_t{ 0U };
thread_local char const* outer_file = nullptr;
thread_local int outer_line = 0;

[[nodiscard]] std::uint64_t now_nsec() noexcept
{
    auto const since_epoch = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count());
}

} // namespace

std::size_t tr_session_lock_depth() noexcept
{
    return depth;
}

char const* tr_session_lock_outer_file() noexcept
{
    return outer_file;
}

int tr_session_lock_outer_line() noexcept
{
    return outer_line;
}

void tr_session_lock::begin_timing() noexcept
{
    counted_ = true;

    if (depth++ == 0U)
    {
        began_nsec_ = now_nsec();
        timed_ = true;
        outer_file = file_;
        outer_line = line_;
        tr_io_trace::on_lock_hold_begin();
    }
}

void tr_session_lock::end_timing() noexcept
{
    if (!counted_)
    {
        return;
    }

    counted_ = false;
    --depth;

    if (!timed_)
    {
        return;
    }

    timed_ = false;
    outer_file = nullptr;
    outer_line = 0;

    auto const end_nsec = now_nsec();
    auto const elapsed_nsec = end_nsec > began_nsec_ ? end_nsec - began_nsec_ : 0U;
    auto const elapsed_usec = elapsed_nsec / 1000U;

    // The hold lands in the histogram either way; a slow one is also reported
    // with what happened inside it (disk ops begun, a backtrace at release).
    tr_io_trace::report_lock_hold(file_, line_, elapsed_usec, tr_io_trace::ops_in_current_hold());
}
