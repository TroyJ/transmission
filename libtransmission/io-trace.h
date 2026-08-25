// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

/**
 * Opt-in latency tracing for disk I/O and session-lock hold times.
 *
 * This exists because "the app froze" is not a debuggable statement. A freeze is
 * always the product of two things: an operation that took a long time, and the
 * fact that it was performed somewhere that blocks everything else. This facility
 * measures both halves separately:
 *
 *   - every `tr_sys_file_*` call is timed (see file-posix.cc), and
 *   - every hold of the session mutex is timed (see session-lock.h).
 *
 * Tracing is off unless the `TR_TRACE_IO` environment variable is set to a
 * nonzero value, and costs nothing when off -- `enabled()` reads a plain bool
 * that is false until a dynamic initializer says otherwise.
 *
 * Environment:
 *   TR_TRACE_IO=1            enable tracing
 *   TR_TRACE_IO_MS=100       log individual operations at or above this many ms
 *   TR_TRACE_IO_DUMP_SEC=60  log the latency histogram this often; 0 disables
 *
 * Example:
 *   TR_TRACE_IO=1 TR_TRACE_IO_MS=250 \
 *     _build/transmission/macosx/Transmission.app/Contents/MacOS/Transmission
 */
namespace tr_io_trace
{

enum class Op : std::uint8_t
{
    Open,
    Close,
    Read,
    Write,
    Truncate,
    Preallocate,
    LockHold,
};

inline auto constexpr NumOps = std::size_t{ 7U };

namespace detail
{
// Zero-initialized before any dynamic initializer runs, so tracing simply
// reads as "off" if some static ctor manages to do I/O before we are ready.
extern bool trace_enabled;
extern std::uint64_t trace_threshold_usec;
} // namespace detail

[[nodiscard]] inline bool enabled() noexcept
{
    return detail::trace_enabled;
}

[[nodiscard]] inline std::uint64_t threshold_usec() noexcept
{
    return detail::trace_threshold_usec;
}

[[nodiscard]] char const* op_name(Op op) noexcept;

/** Best-effort path for an open descriptor. Empty if it can't be resolved. */
[[nodiscard]] std::string path_for_fd(int fd);

void record(Op op, std::uint64_t elapsed_usec, int fd, std::uint64_t offset, std::uint64_t size, std::string_view note);

/** Human-readable latency histogram for every op seen so far. */
[[nodiscard]] std::string dump();

void reset();

/**
 * RAII stopwatch. Times the enclosing scope and reports it to `record()`.
 *
 * `note` is used in place of the descriptor's path when the path can't be
 * recovered from the fd -- e.g. `tr_sys_file_open()`, where there is no fd yet.
 */
class Scope
{
public:
    Scope(Op op, int fd, std::uint64_t offset, std::uint64_t size, std::string_view note = {}) noexcept
        : op_{ op }
        , fd_{ fd }
        , offset_{ offset }
        , size_{ size }
        , enabled_{ enabled() }
    {
        if (!enabled_)
        {
            return;
        }

        // A close() invalidates the fd, so resolve its path while we still can.
        // Only done for the rare ops; never on the read/write hot path.
        note_ = op == Op::Close && std::empty(note) ? path_for_fd(fd) : std::string{ note };
        began_ = std::chrono::steady_clock::now();
    }

    ~Scope()
    {
        if (!enabled_)
        {
            return;
        }

        // Recording formats strings, so it can throw. A diagnostic must never
        // be the reason a file operation fails, so swallow it.
        try
        {
            auto const elapsed = std::chrono::steady_clock::now() - began_;
            auto const usec = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
            record(op_, static_cast<std::uint64_t>(usec < 0 ? 0 : usec), fd_, offset_, size_, note_);
        }
        catch (...)
        {
        }
    }

    Scope(Scope const&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope const&) = delete;
    Scope& operator=(Scope&&) = delete;

private:
    Op op_;
    int fd_;
    std::uint64_t offset_;
    std::uint64_t size_;
    bool enabled_;
    std::string note_;
    std::chrono::steady_clock::time_point began_;
};

} // namespace tr_io_trace
