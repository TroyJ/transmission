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

[[nodiscard]] std::uint64_t now_nsec() noexcept
{
    auto const since_epoch = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count());
}

} // namespace

void tr_session_lock::begin_timing() noexcept
{
    if (!tr_io_trace::enabled())
    {
        return;
    }

    counted_ = true;

    if (depth++ == 0U)
    {
        began_nsec_ = now_nsec();
        timed_ = true;
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

    auto const end_nsec = now_nsec();
    auto const elapsed_nsec = end_nsec > began_nsec_ ? end_nsec - began_nsec_ : 0U;
    auto const elapsed_usec = elapsed_nsec / 1000U;

    // Only pay for the string when the hold is slow enough to be logged. Every
    // other hold just lands in the histogram, and there are a great many of them.
    auto site = std::string{};
    if (file_ != nullptr && elapsed_usec >= tr_io_trace::threshold_usec())
    {
        auto const* const sep = std::strrchr(file_, '/');
        site = fmt::format("{:s}:{:d}", sep != nullptr ? sep + 1 : file_, line_);
    }

    tr_io_trace::record(tr_io_trace::Op::LockHold, elapsed_usec, -1, 0U, 0U, site);
}
