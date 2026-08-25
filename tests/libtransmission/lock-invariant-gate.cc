// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

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
    }
};

[[maybe_unused]] auto const* const registered = ::testing::AddGlobalTestEnvironment(new LockInvariantGate{});

} // namespace
