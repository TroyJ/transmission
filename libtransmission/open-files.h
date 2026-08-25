// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <cstddef> // for size_t
#include <cstdint> // for uintX_t
#include <functional>
#include <optional>
#include <string_view>
#include <utility>

#include "libtransmission/transmission.h"

#include "libtransmission/file.h" // tr_sys_file_t
#include "libtransmission/lru-cache.h"

// A pool of open files that are cached while reading / writing torrents' data
class tr_open_files
{
public:
    enum class Preallocation : uint8_t
    {
        None,
        Sparse,
        Full
    };

    [[nodiscard]] std::optional<tr_sys_file_t> get(tr_torrent_id_t tor_id, tr_file_index_t file_num, bool writable);

    [[nodiscard]] std::optional<tr_sys_file_t> get(
        tr_torrent_id_t tor_id,
        tr_file_index_t file_num,
        bool writable,
        std::string_view filename,
        Preallocation allocation,
        uint64_t file_size);

    void close_all();
    void close_torrent(tr_torrent_id_t tor_id);
    void close_file(tr_torrent_id_t tor_id, tr_file_index_t file_num);

    /**
     * Routes descriptor closes somewhere other than this thread.
     *
     * `close()` is not the cheap bookkeeping call it looks like: on a stalled
     * exFAT/FSKit volume it flushes the file's dirty pages and has been measured
     * taking **144 seconds**. Closes happen on the session thread -- on LRU
     * eviction, on torrent completion, on removal -- so left inline they freeze
     * the app just as the writes used to. The session points this at the disk
     * write worker, whose FIFO ordering also guarantees a file's queued writes
     * are issued before it is closed.
     *
     * Pass `nullptr` to go back to closing inline. The handler must outlive this
     * pool, or be cleared before it is destroyed.
     */
    void set_close_handler(std::function<void(tr_sys_file_t)> handler);

private:
    using Key = std::pair<tr_torrent_id_t, tr_file_index_t>;

    [[nodiscard]] static Key make_key(tr_torrent_id_t tor_id, tr_file_index_t file_num) noexcept
    {
        return std::make_pair(tor_id, file_num);
    }

    struct Val
    {
        Val() noexcept = default;
        Val(Val const&) = delete;
        Val& operator=(Val const&) = delete;
        Val(Val&& that) noexcept
        {
            *this = std::move(that);
        }
        Val& operator=(Val&& that) noexcept
        {
            std::swap(this->fd_, that.fd_);
            std::swap(this->writable_, that.writable_);
            std::swap(this->owner_, that.owner_);
            return *this;
        }
        ~Val();

        tr_sys_file_t fd_ = TR_BAD_SYS_FILE;
        bool writable_ = false;
        tr_open_files* owner_ = nullptr; // for close_handler_; see set_close_handler()
    };

    static constexpr size_t MaxOpenFiles = 32U;
    tr_lru_cache<Key, Val, MaxOpenFiles> pool_;
    std::function<void(tr_sys_file_t)> close_handler_;
};
