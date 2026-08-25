// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <cerrno> // EINVAL
#include <cstddef>
#include <cstdint> // uint8_t
#include <iterator> // std::distance(), std::next(), std::prev()
#include <memory>
#include <numeric> // std::accumulate()
#include <utility> // std::make_pair()
#include <vector>

#include <fmt/format.h>

#include "libtransmission/transmission.h"

#include "libtransmission/cache.h"
#include "libtransmission/disk-write-worker.h"
#include "libtransmission/inout.h"
#include "libtransmission/session.h"
#include "libtransmission/log.h"
#include "libtransmission/torrent.h"
#include "libtransmission/torrents.h"
#include "libtransmission/tr-assert.h"

Cache::Key Cache::make_key(tr_torrent const& tor, tr_block_info::Location const loc) noexcept
{
    return std::make_pair(tor.id(), loc.block);
}

Cache::CIter Cache::find_span_end(CIter const& span_begin, CIter const& end) noexcept
{
    static constexpr auto NotAdjacent = [](CacheBlock const& block1, CacheBlock const& block2)
    {
        return block1.key.first != block2.key.first || block1.key.second + 1 != block2.key.second;
    };
    auto const span_end = std::adjacent_find(span_begin, end, NotAdjacent);
    return span_end == end ? end : std::next(span_end);
}

std::pair<Cache::CIter, Cache::CIter> Cache::find_biggest_span(CIter const& begin, CIter const& end) noexcept
{
    auto biggest_begin = begin;
    auto biggest_end = begin;
    auto biggest_len = std::distance(biggest_begin, biggest_end);

    for (auto span_begin = begin; span_begin < end;)
    {
        auto span_end = find_span_end(span_begin, end);

        if (auto const len = std::distance(span_begin, span_end); len > biggest_len)
        {
            biggest_begin = span_begin;
            biggest_end = span_end;
            biggest_len = len;
        }

        span_begin = span_end;
    }

    return { biggest_begin, biggest_end };
}

int Cache::write_contiguous(CIter const& begin, CIter const& end)
{
    // Always copy into one buffer. The old code aliased a single block's
    // storage to avoid a memcpy, which is no longer safe: the worker outlives
    // this call, so it needs bytes of its own.
    auto const buflen = std::accumulate(
        begin,
        end,
        size_t{},
        [](size_t sum, auto const& block) { return sum + std::size(*block.buf); });

    auto buf = std::vector<uint8_t>{};
    buf.resize(buflen);
    auto* walk = std::data(buf);
    for (auto iter = begin; iter != end; ++iter)
    {
        TR_ASSERT(begin->key.first == iter->key.first);
        TR_ASSERT(begin->key.second + std::distance(begin, iter) == iter->key.second);
        walk = std::copy_n(std::data(*iter->buf), std::size(*iter->buf), walk);
    }
    TR_ASSERT(std::data(buf) + std::size(buf) == walk);

    auto const& [torrent_id, block] = begin->key;
    auto* const tor = torrents_.get(torrent_id);
    if (tor == nullptr)
    {
        return EINVAL;
    }

    auto const loc = tor->block_loc(block);

    // Resolving and opening the files must happen here -- it touches the
    // torrent's layout and the session's fd pool, neither thread-safe -- but
    // it is microseconds where the write is not. Only the write moves.
    auto chunks = std::vector<tr_disk_write_worker::Chunk>{};
    if (auto const err = tr_ioPrepareWrite(*tor, loc, buflen, chunks); err != 0)
    {
        tr_ioReportWriteError(*tor, err);
        return err;
    }

    auto const job_id = next_job_id_++;
    mark_in_flight(job_id, begin, end);

    auto job = tr_disk_write_worker::Job{};
    job.chunks = std::move(chunks);
    job.data = std::move(buf);
    job.on_done = [this, job_id, torrent_id = torrent_id](int const err)
    {
        // Runs on the worker thread; get back onto the session thread before
        // touching any of this.
        session_.run_in_session_thread([this, job_id, torrent_id, err]() { on_write_done(job_id, torrent_id, err); });
    };

    ++disk_writes_;
    disk_write_bytes_ += buflen;

    write_worker_.add(std::move(job));
    return 0;
}

void Cache::mark_in_flight(uint64_t const job_id, CIter const& begin, CIter const& end)
{
    auto& blocks = in_flight_[job_id];
    blocks.reserve(std::distance(begin, end));
    for (auto iter = begin; iter != end; ++iter)
    {
        auto& moved = blocks.emplace_back();
        moved.key = iter->key;
        moved.buf = std::move(const_cast<CacheBlock&>(*iter).buf);
    }
}

void Cache::on_write_done(uint64_t const job_id, tr_torrent_id_t const tor_id, int const error_code)
{
    in_flight_.erase(job_id);

    if (error_code != 0)
    {
        if (auto* const tor = torrents_.get(tor_id); tor != nullptr)
        {
            tr_ioReportWriteError(*tor, error_code);
        }
    }
}

Cache::BlockData const* Cache::find_in_flight(Key const& key) const noexcept
{
    // Newest first: a block can be re-received while an older copy is still
    // being written, and the newer bytes are the ones callers should see.
    for (auto iter = std::rbegin(in_flight_); iter != std::rend(in_flight_); ++iter)
    {
        auto const& blocks = iter->second;
        auto const found = std::lower_bound(std::begin(blocks), std::end(blocks), key, CompareCacheBlockByKey);
        if (found != std::end(blocks) && found->key == key && found->buf)
        {
            return found->buf.get();
        }
    }

    return nullptr;
}

void Cache::drain()
{
    // Once this returns the bytes are on disk, which is all any caller of
    // drain() needs. The completion callbacks that tidy up `in_flight_` and
    // report errors are posted to the session thread and land on the next turn
    // of the event loop; nothing waits on them.
    write_worker_.drain();
}

bool Cache::is_write_backlogged() const noexcept
{
    return write_worker_.pending_bytes() >= MaxInFlightBytes;
}

int Cache::set_limit(Memory const max_size)
{
    max_blocks_ = get_max_blocks(max_size);
    tr_logAddDebug(fmt::format("Maximum cache size set to {} ({} blocks)", max_size.to_string(), max_blocks_));

    return cache_trim();
}

Cache::Cache(tr_session& session, tr_torrents const& torrents, Memory const max_size)
    : session_{ session }
    , torrents_{ torrents }
    , max_blocks_{ get_max_blocks(max_size) }
{
}

Cache::~Cache()
{
    // Blocks already handed off must reach the disk before we go away. The
    // worker's own destructor drains too, but doing it here keeps the ordering
    // explicit and lets the in-flight bookkeeping unwind first.
    write_worker_.drain();
}

// ---

int Cache::write_block(tr_torrent_id_t const tor_id, tr_block_index_t const block, std::unique_ptr<BlockData> writeme)
{
    // A cache size of zero means "don't hold anything back", not "write on the
    // session thread": cache_trim() below flushes immediately when max_blocks_
    // is 0, and that flush now goes to the write worker like any other.
    // https://github.com/transmission/transmission/pull/5668
    auto const key = Key{ tor_id, block };
    auto iter = std::lower_bound(std::begin(blocks_), std::end(blocks_), key, CompareCacheBlockByKey);
    if (iter == std::end(blocks_) || iter->key != key)
    {
        iter = blocks_.emplace(iter);
        iter->key = key;
    }

    iter->buf = std::move(writeme);

    ++cache_writes_;
    cache_write_bytes_ += std::size(*iter->buf);

    return cache_trim();
}

Cache::CIter Cache::get_block(tr_torrent const& tor, tr_block_info::Location const& loc) noexcept
{
    if (auto const [begin, end] = std::equal_range(
            std::begin(blocks_),
            std::end(blocks_),
            make_key(tor, loc),
            CompareCacheBlockByKey);
        begin < end)
    {
        return begin;
    }

    return std::end(blocks_);
}

bool Cache::copy_cached_block(tr_torrent const& tor, tr_block_info::Location const& loc, size_t len, uint8_t* setme) const
{
    auto const key = make_key(tor, loc);
    auto const iter = std::lower_bound(std::begin(blocks_), std::end(blocks_), key, CompareCacheBlockByKey);
    if (iter != std::end(blocks_) && iter->key == key && iter->buf)
    {
        std::copy_n(std::begin(*iter->buf), std::min(len, std::size(*iter->buf)), setme);
        return true;
    }

    // Not in the writable cache -- but it may be mid-flight to the disk, and a
    // block we have received must be readable straight away.
    if (auto const* const buf = find_in_flight(key); buf != nullptr)
    {
        std::copy_n(std::begin(*buf), std::min(len, std::size(*buf)), setme);
        return true;
    }

    return false;
}

int Cache::read_block(tr_torrent const& tor, tr_block_info::Location const& loc, size_t len, uint8_t* setme)
{
    if (auto const iter = get_block(tor, loc); iter != std::end(blocks_) && iter->buf)
    {
        std::copy_n(std::begin(*iter->buf), len, setme);
        return {};
    }

    // A block that is still being written is not on disk yet, so serve it from
    // the in-flight set rather than reading a hole.
    if (auto const* const buf = find_in_flight(make_key(tor, loc)); buf != nullptr)
    {
        std::copy_n(std::begin(*buf), std::min(len, std::size(*buf)), setme);
        return {};
    }

    return tr_ioRead(tor, loc, len, setme);
}

// ---

int Cache::flush_span(CIter const& begin, CIter const& end)
{
    // write_contiguous() moves each block's buffer out into the job, so the
    // entries left behind are husks. Erase them once, at the end: erasing as we
    // go would invalidate the iterators this loop is walking.
    auto err = int{};

    for (auto span_begin = begin; span_begin < end;)
    {
        auto const span_end = find_span_end(span_begin, end);

        if (err = write_contiguous(span_begin, span_end); err != 0)
        {
            break;
        }

        span_begin = span_end;
    }

    blocks_.erase(begin, end);
    return err;
}

int Cache::flush_file(tr_torrent const& tor, tr_file_index_t const file)
{
    auto const tor_id = tor.id();
    auto const [block_begin, block_end] = tor.block_span_for_file(file);

    return flush_span(
        std::lower_bound(std::begin(blocks_), std::end(blocks_), std::make_pair(tor_id, block_begin), CompareCacheBlockByKey),
        std::lower_bound(std::begin(blocks_), std::end(blocks_), std::make_pair(tor_id, block_end), CompareCacheBlockByKey));
}

int Cache::flush_torrent(tr_torrent_id_t const tor_id)
{
    return flush_span(
        std::lower_bound(std::begin(blocks_), std::end(blocks_), std::make_pair(tor_id, 0), CompareCacheBlockByKey),
        std::lower_bound(std::begin(blocks_), std::end(blocks_), std::make_pair(tor_id + 1, 0), CompareCacheBlockByKey));
}

int Cache::flush_all()
{
    return flush_span(std::begin(blocks_), std::end(blocks_));
}

int Cache::flush_biggest()
{
    auto const [begin, end] = find_biggest_span(std::begin(blocks_), std::end(blocks_));

    if (begin == end) // nothing to flush
    {
        return 0;
    }

    auto const err = write_contiguous(begin, end);

    // Erase either way: on success the buffers have moved into the job, and on
    // failure the torrent has been stopped and holding the husks helps nobody.
    blocks_.erase(begin, end);
    return err;
}

int Cache::cache_trim()
{
    while (std::size(blocks_) > max_blocks_)
    {
        if (auto const err = flush_biggest(); err != 0)
        {
            return err;
        }
    }

    return 0;
}
