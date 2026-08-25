// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <fcntl.h>
#include <climits> // PATH_MAX
#include <unistd.h>
#endif

#include <fmt/format.h>

#include "libtransmission/transmission.h"

#include "libtransmission/io-trace.h"

using namespace std::literals;

namespace tr_io_trace
{
namespace detail
{
bool trace_enabled = false;
std::uint64_t trace_threshold_usec = 0U;
} // namespace detail

namespace
{

// Bucket i holds samples in [2^(i-1), 2^i) microseconds; bucket 0 is exactly 0.
// 24 buckets reaches ~8.4 s, which is past the point where anything is "slow".
auto constexpr NumBuckets = std::size_t{ 24U };

struct OpStats
{
    std::atomic<std::uint64_t> count{ 0U };
    std::atomic<std::uint64_t> total_usec{ 0U };
    std::atomic<std::uint64_t> max_usec{ 0U };
    std::array<std::atomic<std::uint64_t>, NumBuckets> buckets{};
};

auto stats = std::array<OpStats, NumOps>{};

auto dump_interval = std::chrono::seconds{ 0 };
auto next_dump = std::atomic<std::chrono::steady_clock::rep>{ 0 };

[[nodiscard]] constexpr std::size_t bucket_for(std::uint64_t usec) noexcept
{
    auto idx = std::size_t{ 0U };
    while (usec != 0U && idx + 1U < NumBuckets)
    {
        usec >>= 1U;
        ++idx;
    }
    return idx;
}

[[nodiscard]] std::string bucket_label(std::size_t idx)
{
    if (idx == 0U)
    {
        return "0"s;
    }

    auto const usec = std::uint64_t{ 1U } << (idx - 1U);
    if (usec < 1000U)
    {
        return fmt::format("{}us", usec);
    }
    if (usec < 1000000U)
    {
        return fmt::format("{}ms", usec / 1000U);
    }
    return fmt::format("{}s", usec / 1000000U);
}

[[nodiscard]] std::uint64_t env_number(char const* key, std::uint64_t fallback)
{
    auto const* const val = std::getenv(key);
    if (val == nullptr || *val == '\0')
    {
        return fallback;
    }

    auto const num = std::strtoull(val, nullptr, 10);
    return num;
}

// io-trace deliberately does not go through tr_logAdd*(). The log defaults to
// TR_LOG_ERROR, so trace output would be invisible unless the user also raised
// the level, and warn-level messages are capped at 30 repeats per call site --
// which is precisely the wrong behaviour for something counting slow writes.
// Instead we own a sink: stderr by default, or TR_TRACE_IO_FILE if set (useful
// for the .app, where stderr goes nowhere you can read).
std::mutex sink_mutex;
std::FILE* sink = nullptr;

void write_line(std::string const& line)
{
    auto const lock = std::lock_guard{ sink_mutex };

    if (sink == nullptr)
    {
        return;
    }

    auto const now = std::time(nullptr);
    auto tm_buf = std::tm{};
    auto stamp = std::array<char, 32>{};
    if (localtime_r(&now, &tm_buf) != nullptr)
    {
        static_cast<void>(std::strftime(std::data(stamp), std::size(stamp), "%H:%M:%S", &tm_buf));
    }

    static_cast<void>(std::fprintf(sink, "%s %s\n", std::data(stamp), line.c_str()));
    static_cast<void>(std::fflush(sink));
}

[[nodiscard]] bool env_is_on(char const* key)
{
    auto const* const val = std::getenv(key);
    if (val == nullptr || *val == '\0')
    {
        return false;
    }

    auto const sv = std::string_view{ val };
    return sv != "0"sv && sv != "no"sv && sv != "off"sv && sv != "false"sv;
}

void maybe_dump(std::chrono::steady_clock::time_point now)
{
    if (dump_interval.count() == 0)
    {
        return;
    }

    auto const now_ticks = now.time_since_epoch().count();
    auto due = next_dump.load(std::memory_order_relaxed);
    if (now_ticks < due)
    {
        return;
    }

    auto const next = (now + dump_interval).time_since_epoch().count();
    if (!next_dump.compare_exchange_strong(due, next, std::memory_order_relaxed))
    {
        return; // another thread is dumping
    }

    write_line(dump());
}

struct Initializer
{
    Initializer()
    {
        detail::trace_enabled = env_is_on("TR_TRACE_IO");
        if (!detail::trace_enabled)
        {
            return;
        }

        detail::trace_threshold_usec = env_number("TR_TRACE_IO_MS", 100U) * 1000U;

        auto const* const path = std::getenv("TR_TRACE_IO_FILE");
        sink = path != nullptr && *path != '\0' ? std::fopen(path, "ae") : stderr;
        if (sink == nullptr)
        {
            sink = stderr;
        }

        dump_interval = std::chrono::seconds{ static_cast<long long>(env_number("TR_TRACE_IO_DUMP_SEC", 60U)) };
        next_dump.store((std::chrono::steady_clock::now() + dump_interval).time_since_epoch().count());
    }

    // A final dump at exit, so a short-lived run still reports its histogram
    // rather than only the periodic ones it never lived long enough to emit.
    // `stats` and `sink` are declared above this object in the same translation
    // unit, so they outlive it.
    ~Initializer()
    {
        if (!detail::trace_enabled)
        {
            return;
        }

        try
        {
            write_line(dump());
        }
        catch (...)
        {
        }

        auto const lock = std::lock_guard{ sink_mutex };
        if (sink != nullptr && sink != stderr)
        {
            static_cast<void>(std::fclose(sink));
        }
        sink = nullptr;
    }
};

auto const initializer = Initializer{};

} // namespace

char const* op_name(Op op) noexcept
{
    static auto constexpr Names = std::array<char const*, NumOps>{
        "open", "close", "read", "write", "truncate", "preallocate", "lock-hold",
    };

    auto const idx = static_cast<std::size_t>(op);
    return idx < NumOps ? Names[idx] : "?";
}

std::string path_for_fd(int fd)
{
    if (fd < 0)
    {
        return {};
    }

#ifdef __APPLE__
    auto buf = std::array<char, PATH_MAX>{};
    if (fcntl(fd, F_GETPATH, std::data(buf)) != -1)
    {
        return std::string{ std::data(buf) };
    }
#elif defined(__linux__)
    auto buf = std::array<char, PATH_MAX>{};
    auto const link = fmt::format("/proc/self/fd/{}", fd);
    if (auto const n = readlink(link.c_str(), std::data(buf), std::size(buf) - 1U); n > 0)
    {
        return std::string{ std::data(buf), static_cast<std::size_t>(n) };
    }
#endif

    return {};
}

void record(Op op, std::uint64_t elapsed_usec, int fd, std::uint64_t offset, std::uint64_t size, std::string_view note)
{
    auto const idx = static_cast<std::size_t>(op);
    if (idx >= NumOps)
    {
        return;
    }

    auto& st = stats[idx];
    st.count.fetch_add(1U, std::memory_order_relaxed);
    st.total_usec.fetch_add(elapsed_usec, std::memory_order_relaxed);
    st.buckets[bucket_for(elapsed_usec)].fetch_add(1U, std::memory_order_relaxed);

    auto prev = st.max_usec.load(std::memory_order_relaxed);
    while (elapsed_usec > prev && !st.max_usec.compare_exchange_weak(prev, elapsed_usec, std::memory_order_relaxed))
    {
        // retry with the updated `prev`
    }

    if (elapsed_usec >= threshold_usec())
    {
        auto where = std::string{ note };
        if (std::empty(where))
        {
            where = path_for_fd(fd);
        }

        if (op == Op::LockHold)
        {
            write_line(
                std::empty(where) ?
                    fmt::format("[io-trace] slow lock-hold: {:.3f}s", elapsed_usec / 1e6) :
                    fmt::format("[io-trace] slow lock-hold: {:.3f}s held from {:s}", elapsed_usec / 1e6, where));
        }
        else
        {
            write_line(
                fmt::format(
                    "[io-trace] slow {:s}: {:.3f}s size={} offset={} {:s}",
                    op_name(op),
                    elapsed_usec / 1e6,
                    size,
                    offset,
                    where));
        }
    }

    maybe_dump(std::chrono::steady_clock::now());
}

std::string dump()
{
    auto out = std::string{ "[io-trace] latency histogram (count per bucket, bucket = lower bound)\n" };

    // which buckets are worth printing
    auto lo = NumBuckets;
    auto hi = std::size_t{ 0U };
    for (auto const& st : stats)
    {
        for (auto i = std::size_t{ 0U }; i < NumBuckets; ++i)
        {
            if (st.buckets[i].load(std::memory_order_relaxed) != 0U)
            {
                lo = std::min(lo, i);
                hi = std::max(hi, i);
            }
        }
    }

    if (lo > hi)
    {
        return out + "  (nothing recorded)";
    }

    out += fmt::format("  {:<12}{:>9}{:>11}{:>11}", "op", "count", "mean", "max");
    for (auto i = lo; i <= hi; ++i)
    {
        out += fmt::format("{:>9}", bucket_label(i));
    }
    out += '\n';

    for (auto op = std::size_t{ 0U }; op < NumOps; ++op)
    {
        auto const& st = stats[op];
        auto const count = st.count.load(std::memory_order_relaxed);
        if (count == 0U)
        {
            continue;
        }

        auto const total = st.total_usec.load(std::memory_order_relaxed);
        out += fmt::format(
            "  {:<12}{:>9}{:>10.3f}s{:>10.3f}s",
            op_name(static_cast<Op>(op)),
            count,
            static_cast<double>(total) / count / 1e6,
            st.max_usec.load(std::memory_order_relaxed) / 1e6);

        for (auto i = lo; i <= hi; ++i)
        {
            auto const n = st.buckets[i].load(std::memory_order_relaxed);
            out += n != 0U ? fmt::format("{:>9}", n) : fmt::format("{:>9}", ".");
        }
        out += '\n';
    }

    if (!std::empty(out) && out.back() == '\n')
    {
        out.pop_back();
    }

    return out;
}

void reset()
{
    for (auto& st : stats)
    {
        st.count.store(0U, std::memory_order_relaxed);
        st.total_usec.store(0U, std::memory_order_relaxed);
        st.max_usec.store(0U, std::memory_order_relaxed);
        for (auto& bucket : st.buckets)
        {
            bucket.store(0U, std::memory_order_relaxed);
        }
    }
}

} // namespace tr_io_trace
