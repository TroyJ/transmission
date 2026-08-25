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
 *   TR_TRACE_IO_LOCKED_BT=3  print a backtrace for the first N I/O calls made
 *                            while the session lock is held, per (op, lock site)
 *   TR_TRACE_IO_ABORT=1      abort() on the first I/O call made under the lock,
 *                            for hunting a regression under a debugger or in a
 *                            test run (known offenders remain -- see
 *                            docs/HANDOVER-disk-stalls-and-2a.md §9.4 -- so this
 *                            is a tool, not yet a CI gate)
 *
 * Lock invariant: any disk operation begun while the calling thread holds the
 * session mutex is a freeze waiting for a slow disk, regardless of how long it
 * took this time. Every such call is counted separately in the histogram (as
 * `<op>@lock`) and the first few per call site are logged with a backtrace, so
 * the complete list of offenders falls out of one ordinary run rather than
 * out of a stress test that happens to catch them being slow.
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
    Path, // stat/rename/remove/mkdir/opendir: metadata ops, which queue behind writes on a stalled volume
    Wait, // the session thread blocking on the write worker (Cache::drain)
    LockHold,
};

inline auto constexpr NumOps = std::size_t{ 9U };

namespace detail
{
// Zero-initialized before any dynamic initializer runs, so tracing simply
// reads as "off" if some static ctor manages to do I/O before we are ready.
extern bool trace_enabled;
extern std::uint64_t trace_threshold_usec;
} // namespace detail

/**
 * The counters are always maintained, tracing on or off: two clock reads per
 * disk op and per lock hold, and a few relaxed atomics. `enabled()` only
 * gates the *output* (per-op log lines, histogram dumps, backtraces). This is
 * what lets the RPC `session-stats` report disk health on a production
 * build without anyone having restarted with TR_TRACE_IO.
 */
struct Snapshot
{
    std::uint64_t lock_hold_max_usec = 0U; // longest session-lock hold so far
    std::uint64_t slow_op_count = 0U; // disk ops that took >= 1 s
    std::uint64_t worst_op_usec = 0U; // the slowest disk op so far...
    Op worst_op = Op::Open; // ...and what it was
    std::uint64_t pending_write_bytes = 0U; // queued for the write worker right now
};

[[nodiscard]] Snapshot snapshot() noexcept;

/**
 * How many times `op` has run on the data volume while the session lock was
 * held (config-dir I/O is not counted). The test suite asserts this stays at
 * zero for the unbounded ops -- read, write, close, wait -- which is the
 * regression gate for every freeze fixed in docs/HANDOVER-disk-stalls-and-2a.md.
 */
[[nodiscard]] std::uint64_t locked_count(Op op) noexcept;

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

void record(
    Op op,
    std::uint64_t elapsed_usec,
    int fd,
    std::uint64_t offset,
    std::uint64_t size,
    std::string_view note,
    bool under_session_lock = false);

/**
 * Called when an I/O op is about to run while the session lock is held.
 * Logs the op, the lock's call site, and a backtrace (rate-limited per site).
 */
void report_locked_io(Op op, std::string_view note);

/** True if the calling thread holds the session mutex. */
[[nodiscard]] bool session_lock_held() noexcept;

/**
 * Records a named quantity (e.g. bytes queued for the write worker). The
 * histogram dump prints each gauge's latest and highest value.
 */
void gauge(std::string_view name, std::uint64_t value) noexcept;

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
        under_lock_ = session_lock_held();

        if (enabled_)
        {
            // A close() invalidates the fd, so resolve its path while we still can.
            // Only done for the rare ops; never on the read/write hot path.
            note_ = op == Op::Close && std::empty(note) ? path_for_fd(fd) : std::string{ note };
        }
        else
        {
            note_ = std::string{ note };
        }

        if (under_lock_)
        {
            // Under the lock the path is always resolved, tracing or not: the
            // config-dir allowlist that keeps the locked counters honest (and
            // the test-suite gate quiet) needs it.
            if (std::empty(note_))
            {
                note_ = path_for_fd(fd);
            }
            report_locked_io(op, note_);
        }

        began_ = std::chrono::steady_clock::now();
    }

    ~Scope()
    {
        // Recording formats strings, so it can throw. A diagnostic must never
        // be the reason a file operation fails, so swallow it.
        try
        {
            auto const elapsed = std::chrono::steady_clock::now() - began_;
            auto const usec = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
            record(op_, static_cast<std::uint64_t>(usec < 0 ? 0 : usec), fd_, offset_, size_, note_, under_lock_);
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
    bool under_lock_ = false;
    std::string note_;
    std::chrono::steady_clock::time_point began_;
};

} // namespace tr_io_trace
