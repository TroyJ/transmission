// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <cstddef> // for size_t
#include <cstdint> // for intX_t, uintX_t
#include <functional>
#include <map>
#include <memory> // for std::unique_ptr
#include <utility> // for std::pair
#include <vector>

#include <small/vector.hpp>

#include "libtransmission/transmission.h"

#include "libtransmission/block-info.h"
#include "libtransmission/disk-write-worker.h"
#include "libtransmission/values.h"

class tr_torrents;
struct tr_torrent;
struct tr_session;

class Cache
{
public:
    using BlockData = small::max_size_vector<uint8_t, tr_block_info::BlockSize>;
    using Memory = libtransmission::Values::Memory;

    Cache(tr_session& session, tr_torrents const& torrents, Memory max_size);
    ~Cache();

    Cache(Cache const&) = delete;
    Cache(Cache&&) = delete;
    Cache& operator=(Cache const&) = delete;
    Cache& operator=(Cache&&) = delete;

    int set_limit(Memory max_size);

    // @return any error code from cacheTrim()
    int write_block(tr_torrent_id_t tor, tr_block_index_t block, std::unique_ptr<BlockData> writeme);

    int read_block(tr_torrent const& tor, tr_block_info::Location const& loc, size_t len, uint8_t* setme);
    // copies the block into `setme` iff it is still in the write cache. Never touches disk.
    [[nodiscard]] bool copy_cached_block(tr_torrent const& tor, tr_block_info::Location const& loc, size_t len, uint8_t* setme)
        const;
    int flush_torrent(tr_torrent_id_t tor_id);
    int flush_file(tr_torrent const& tor, tr_file_index_t file);

    /** Flushes every cached block, for every torrent. Used at shutdown. */
    int flush_all();

    /**
     * Blocks until every flush handed to the write worker has reached the disk.
     *
     * Callers that must see the bytes on disk -- the `.part` rename, the
     * relocation worker, torrent removal, session shutdown -- use this. It is
     * the one place the session thread still waits on the disk, and it happens
     * per file or per torrent rather than per block.
     */
    void drain();

    /**
     * True when the write worker is far enough behind that we should stop
     * asking peers for more blocks.
     *
     * Backpressure travels through the request pipeline, not by blocking:
     * peers can only send what we asked for, so declining to ask is what
     * bounds the cache. See tr_peerMgrGetNextRequests().
     */
    [[nodiscard]] bool is_write_backlogged() const noexcept;

    [[nodiscard]] size_t pending_write_bytes() const noexcept
    {
        return write_worker_.pending_bytes();
    }

    [[nodiscard]] size_t pending_jobs() const noexcept
    {
        return write_worker_.pending_jobs();
    }

    /** Ceiling on bytes handed to the worker but not yet on disk. */
    static constexpr size_t MaxInFlightBytes = 32U * 1024U * 1024U;

    /**
     * Runs `on_session_thread` once everything currently queued has been
     * written -- without blocking the session thread waiting for it.
     *
     * This is how the flush-then-act sequences are kept honest. The worker is
     * FIFO, so a job queued now runs after every write already queued; when it
     * completes, the continuation is posted back to the session thread. Callers
     * get "the bytes are on disk, now do the thing" without a `drain()`.
     */
    void run_after_pending_writes(std::function<void()> on_session_thread);

    /**
     * Closes a descriptor on the write worker instead of here.
     *
     * Installed as tr_open_files' close handler. The worker's FIFO ordering
     * means anything already queued for this file is written before the close
     * lands, and `close()` -- which blocks for as long as a write on a stalled
     * volume -- stops being the session thread's problem.
     */
    void close_fd_async(tr_sys_file_t fd);

    /** Testing only; see tr_disk_write_worker::set_paused(). */
    void set_write_paused(bool paused)
    {
        write_worker_.set_paused(paused);
    }

private:
    using Key = std::pair<tr_torrent_id_t, tr_block_index_t>;

    struct CacheBlock
    {
        Key key;
        std::unique_ptr<BlockData> buf;
    };

    using Blocks = std::vector<CacheBlock>;
    using CIter = Blocks::const_iterator;

    [[nodiscard]] static Key make_key(tr_torrent const& tor, tr_block_info::Location loc) noexcept;

    [[nodiscard]] static std::pair<CIter, CIter> find_biggest_span(CIter const& begin, CIter const& end) noexcept;

    [[nodiscard]] static CIter find_span_end(CIter const& span_begin, CIter const& end) noexcept;

    // Hands one contiguous span to the write worker. Copies the bytes and
    // opens the files here, on the session thread, then returns without
    // waiting: the `pwrite` itself happens on the worker.
    // @return 0, or an errno if the files could not be opened
    [[nodiscard]] int write_contiguous(CIter const& begin, CIter const& end);

    // @return any error code from writeContiguous()
    [[nodiscard]] int flush_span(CIter const& begin, CIter const& end);

    // @return any error code from writeContiguous()
    [[nodiscard]] int flush_biggest();

    // @return any error code from writeContiguous()
    [[nodiscard]] int cache_trim();

    [[nodiscard]] static constexpr size_t get_max_blocks(Memory const max_size) noexcept
    {
        return max_size.base_quantity() / tr_block_info::BlockSize;
    }

    [[nodiscard]] CIter get_block(tr_torrent const& tor, tr_block_info::Location const& loc) noexcept;

    // Moves [begin, end) out of blocks_ and into in_flight_ under `job_id`.
    // They stay readable there until the write commits.
    void mark_in_flight(uint64_t job_id, CIter const& begin, CIter const& end);

    // Session-thread half of a completed write: drop the blocks, report errors.
    void on_write_done(uint64_t job_id, tr_torrent_id_t tor_id, int error_code);

    // Serves a block still being written. Returns nullptr if not in flight.
    [[nodiscard]] BlockData const* find_in_flight(Key const& key) const noexcept;

    tr_session& session_;
    tr_torrents const& torrents_;

    tr_disk_write_worker write_worker_;

    // Blocks handed to the worker, keyed by job. Kept until the write commits
    // so that peer requests and hash checks still see them (a block that has
    // been received must be readable immediately, whether or not it has
    // reached the platter yet).
    std::map<uint64_t, Blocks> in_flight_;
    uint64_t next_job_id_ = 0U;

    Blocks blocks_;
    size_t max_blocks_ = 0;

    mutable size_t disk_writes_ = 0;
    mutable size_t disk_write_bytes_ = 0;
    mutable size_t cache_writes_ = 0;
    mutable size_t cache_write_bytes_ = 0;

    static constexpr struct
    {
        [[nodiscard]] constexpr bool operator()(Key const& key, CacheBlock const& block) const
        {
            return key < block.key;
        }
        [[nodiscard]] constexpr bool operator()(CacheBlock const& block, Key const& key) const
        {
            return block.key < key;
        }
    } CompareCacheBlockByKey{};
};
