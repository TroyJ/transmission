// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <cstdint>

#include <libtransmission/transmission.h>

#include <libtransmission/io-trace.h>

#include "gtest/gtest.h"

/**
 * The invariant behind every GUI/RPC freeze in docs/HANDOVER-disk-stalls-and-2a.md:
 * no unbounded disk operation runs while the session mutex is held.
 *
 * The io-trace counters are always on, so after the whole suite has run --
 * every add, start, flush, complete, pause, remove, relocate and shutdown
 * path the tests exercise -- the data-volume read/write/close/drain counts
 * under the lock must still be zero. `open` and `path` (stat/mkdir) are
 * deliberately not gated: a handful are still done under the lock, once per
 * file per start, and are tracked in docs/todo.md.
 *
 * Runs as a global environment so it sees the totals, whatever the order.
 *
 * The second half of the invariant -- *nothing on the session thread waits* --
 * is gated here too. Op::SessionThreadRun times every task the session thread
 * runs; a task that blocks on a worker or a condition variable is the freeze,
 * regardless of whether it holds the mutex or touches a disk, and neither of
 * the counters above can see it. See plans/blocking-taxonomy-and-boundary.md
 * for what that class cost before it was measurable.
 */
namespace
{

class LockInvariantGate : public ::testing::Environment
{
public:
    void TearDown() override
    {
        using tr_io_trace::Op;

        for (auto const op : { Op::Write, Op::Read, Op::Close, Op::Wait, Op::Truncate, Op::Preallocate })
        {
            EXPECT_EQ(0U, tr_io_trace::locked_count(op))
                << "disk op '" << tr_io_trace::op_name(op) << "' ran on the data volume while the session lock was held. "
                << "Run with TR_TRACE_IO=1 TR_TRACE_IO_LOCKED_BT=3 for a backtrace of each offender.";
        }

        // A session-thread task may be slow for honest reasons (a big torrent's
        // bookkeeping), so the bar is set where "slow" stops being a plausible
        // explanation and "blocked" starts. Tests that deliberately stall the
        // disk with tr_io_trace::set_injected_delay() must keep the stall off
        // the session thread -- that is the point of the gate.
        static auto constexpr SessionThreadRunMaxUsec = std::uint64_t{ 2U * 1000U * 1000U };
        auto const snap = tr_io_trace::snapshot();
        EXPECT_LT(snap.session_thread_run_max_usec, SessionThreadRunMaxUsec)
            << "a session thread task ran for " << (snap.session_thread_run_max_usec / 1000U)
            << " ms. Something it called waited -- on a worker, a condition variable, or a slow "
            << "syscall -- and everything the session thread serves (RPC, peers, queued commands) "
            << "waited with it. Run with TR_TRACE_IO=1 TR_TRACE_IO_MS=250 to see what it was doing.";
    }
};

[[maybe_unused]] auto const* const registered = ::testing::AddGlobalTestEnvironment(new LockInvariantGate{});

} // namespace
