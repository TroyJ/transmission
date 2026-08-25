// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>

/**
 * RAII guard for the session mutex.
 *
 * Interface-compatible with the `std::unique_lock<std::recursive_mutex>` this
 * replaced, for everything callers actually do with it. What it adds is timing:
 * every outermost hold of the session lock is measured and reported to the
 * io-trace facility (see io-trace.h), so a hold long enough to freeze the GUI
 * and the RPC server shows up as a logged number rather than as a bug report.
 *
 * Only the outermost acquisition on a thread is timed -- the mutex is recursive,
 * and nested holds would otherwise each report the remainder of their parent.
 *
 * Timing is always on (two clock reads per outermost hold); only the logging
 * needs `TR_TRACE_IO`. See io-trace.h.
 *
 * This header is reachable from the public transmission.h, and so from every
 * Objective-C++ client, so it is deliberately kept to <cstdint> and <mutex>.
 * The clock lives entirely in session-lock.cc and the start timestamp is held
 * here as a plain nanosecond count.
 */
/**
 * How deeply the calling thread currently holds the session mutex, and the
 * call site of its outermost hold. Used by io-trace to flag disk I/O done
 * while the lock is held -- the one invariant behind every GUI/RPC freeze.
 */
[[nodiscard]] std::size_t tr_session_lock_depth() noexcept;
[[nodiscard]] char const* tr_session_lock_outer_file() noexcept;
[[nodiscard]] int tr_session_lock_outer_line() noexcept;

class tr_session_lock
{
public:
    // `file`/`line` identify the code that took the lock, so a hold long enough
    // to be reported can name itself. Callers get them for free: the accessors
    // that hand out this guard default them to __builtin_FILE()/__builtin_LINE(),
    // which expand at the call site.
    explicit tr_session_lock(std::recursive_mutex& mutex, char const* file = nullptr, int line = 0)
        : lock_{ mutex }
        , file_{ file }
        , line_{ line }
    {
        begin_timing();
    }

    ~tr_session_lock()
    {
        end_timing();
    }

    // Returned by value from tr_session::unique_lock() as a prvalue, so C++17
    // guaranteed copy elision covers that without a move ctor. Keeping the type
    // immovable is deliberate: the recursion depth it tracks is thread-local, so
    // a guard that changed threads mid-life would corrupt it.
    tr_session_lock(tr_session_lock const&) = delete;
    tr_session_lock(tr_session_lock&&) = delete;
    tr_session_lock& operator=(tr_session_lock const&) = delete;
    tr_session_lock& operator=(tr_session_lock&&) = delete;

    void unlock()
    {
        end_timing();
        lock_.unlock();
    }

    void lock()
    {
        lock_.lock();
        begin_timing();
    }

    [[nodiscard]] bool owns_lock() const noexcept
    {
        return lock_.owns_lock();
    }

private:
    void begin_timing() noexcept;
    void end_timing() noexcept;

    std::unique_lock<std::recursive_mutex> lock_;
    char const* file_ = nullptr;
    int line_ = 0;
    std::uint64_t began_nsec_ = 0U;
    bool counted_ = false; // this guard contributed to the recursion depth
    bool timed_ = false; // ...and it was the outermost one, so it owns the clock
};
