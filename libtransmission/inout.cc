// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <optional>
#include <string_view>

#include <fmt/format.h>

#include "libtransmission/transmission.h"

#include "libtransmission/block-info.h" // tr_block_info
#include "libtransmission/disk-write-worker.h"
#include "libtransmission/crypto-utils.h"
#include "libtransmission/error.h"
#include "libtransmission/file.h"
#include "libtransmission/inout.h"
#include "libtransmission/session.h"
#include "libtransmission/torrent.h"
#include "libtransmission/torrent-files.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-macros.h" // tr_sha1_digest_t
#include "libtransmission/tr-strbuf.h" // tr_pathbuf
#include "libtransmission/utils.h"

using namespace std::literals;

namespace
{

bool read_entire_buf(tr_sys_file_t const fd, uint64_t file_offset, uint8_t* buf, uint64_t buflen, tr_error& error)
{
    while (buflen > 0U)
    {
        auto n_read = uint64_t{};

        if (!tr_sys_file_read_at(fd, buf, buflen, file_offset, &n_read, &error))
        {
            return false;
        }

        buf += n_read;
        buflen -= n_read;
        file_offset += n_read;
    }

    return true;
}

bool write_entire_buf(tr_sys_file_t const fd, uint64_t file_offset, uint8_t const* buf, uint64_t buflen, tr_error& error)
{
    while (buflen > 0U)
    {
        auto n_written = uint64_t{};

        if (!tr_sys_file_write_at(fd, buf, buflen, file_offset, &n_written, &error))
        {
            return false;
        }

        buf += n_written;
        buflen -= n_written;
        file_offset += n_written;
    }

    return true;
}

[[nodiscard]] std::optional<tr_sys_file_t> get_fd(
    tr_session& session,
    tr_open_files& open_files,
    tr_torrent const& tor,
    bool const writable,
    tr_file_index_t const file_index,
    tr_error& error)
{
    auto const tor_id = tor.id();

    // is the file already open in the fd pool?
    if (auto const fd = open_files.get(tor_id, file_index, writable); fd)
    {
        return fd;
    }

    // does the file exist?
    auto const file_size = tor.file_size(file_index);
    auto const prealloc = writable && tor.file_is_wanted(file_index) ? session.preallocationMode() :
                                                                       tr_open_files::Preallocation::None;
    if (auto const found = tor.found_file_path(file_index); found)
    {
        return open_files.get(tor_id, file_index, writable, *found, prealloc, file_size);
    }

    // do we want to create it?
    auto err = ENOENT;
    if (writable)
    {
        auto const base = tor.current_dir();
        auto const suffix = session.isIncompleteFileNamingEnabled() ? tr_torrent_files::PartialFileSuffix : ""sv;
        auto const filename = tr_pathbuf{ base, '/', tor.file_subpath(file_index), suffix };
        if (auto const fd = open_files.get(tor_id, file_index, writable, filename, prealloc, file_size); fd)
        {
            // make a note that we just created a file
            session.add_file_created();
            tor.remember_found_path(file_index, filename);
            return fd;
        }

        err = errno;
    }

    error.set(
        err,
        fmt::format(
            fmt::runtime(_("Couldn't get '{path}': {error} ({error_code})")),
            fmt::arg("path", tor.file_subpath(file_index)),
            fmt::arg("error", tr_strerror(err)),
            fmt::arg("error_code", err)));
    return {};
}

void read_or_write_bytes(
    tr_session& session,
    tr_open_files& open_files,
    tr_torrent const& tor,
    bool const writable,
    tr_file_index_t const file_index,
    uint64_t const file_offset,
    uint8_t* const buf,
    uint64_t const buflen,
    tr_error& error)
{
    TR_ASSERT(file_index < tor.file_count());
    auto const file_size = tor.file_size(file_index);
    TR_ASSERT(file_size == 0U || file_offset < file_size);
    TR_ASSERT(file_offset + buflen <= file_size);
    if (file_size == 0U)
    {
        return;
    }

    auto const fd = get_fd(session, open_files, tor, writable, file_index, error);
    if (!fd || error)
    {
        return;
    }

    auto fmtstr = ""sv;
    if (writable)
    {
        fmtstr = _("Couldn't save '{path}': {error} ({error_code})");
        write_entire_buf(*fd, file_offset, buf, buflen, error);
    }
    else
    {
        fmtstr = _("Couldn't read '{path}': {error} ({error_code})");
        read_entire_buf(*fd, file_offset, buf, buflen, error);
    }

    if (error)
    {
        tr_logAddErrorTor(
            &tor,
            fmt::format(
                fmt::runtime(fmtstr),
                fmt::arg("path", tor.file_subpath(file_index)),
                fmt::arg("error", error.message()),
                fmt::arg("error_code", error.code())));
    }
}

void read_or_write_piece(
    tr_torrent const& tor,
    bool const writable,
    tr_block_info::Location const loc,
    uint8_t* buf,
    uint64_t buflen,
    tr_error& error)
{
    if (loc.piece >= tor.piece_count())
    {
        error.set_from_errno(EINVAL);
        return;
    }

    auto [file_index, file_offset] = tor.file_offset(loc);
    auto& session = *tor.session;
    auto& open_files = session.openFiles();
    while (buflen != 0U && !error)
    {
        auto const bytes_this_pass = std::min(buflen, tor.file_size(file_index) - file_offset);
        read_or_write_bytes(session, open_files, tor, writable, file_index, file_offset, buf, bytes_this_pass, error);
        if (buf != nullptr)
        {
            buf += bytes_this_pass;
        }
        buflen -= bytes_this_pass;
        ++file_index;
        file_offset = 0U;
    }
}

std::optional<tr_sha1_digest_t> recalculate_hash(tr_torrent const& tor, tr_piece_index_t const piece)
{
    TR_ASSERT(piece < tor.piece_count());

    auto sha = tr_sha1{};
    auto buffer = std::array<uint8_t, tr_block_info::BlockSize>{};

    auto& cache = tor.session->cache;
    auto const [begin_byte, end_byte] = tor.block_info().byte_span_for_piece(piece);
    auto const [begin_block, end_block] = tor.block_span_for_piece(piece);
    [[maybe_unused]] auto n_bytes_checked = size_t{};
    for (auto block = begin_block; block < end_block; ++block)
    {
        auto const block_loc = tor.block_loc(block);
        auto const block_len = tor.block_size(block);
        if (auto const success = cache->read_block(tor, block_loc, block_len, std::data(buffer)) == 0; !success)
        {
            return {};
        }

        auto begin = std::data(buffer);
        auto end = begin + block_len;

        // handle edge case where blocks aren't on piece boundaries:
        if (block == begin_block) // `block` may begin before `piece` does
        {
            begin += (begin_byte - block_loc.byte);
        }
        if (block + 1U == end_block) // `block` may end after `piece` does
        {
            end -= (block_loc.byte + block_len - end_byte);
        }

        sha.add(begin, end - begin);
        n_bytes_checked += (end - begin);
    }

    TR_ASSERT(tor.piece_size(piece) == n_bytes_checked);
    return sha.finish();
}

} // namespace

int tr_ioRead(tr_torrent const& tor, tr_block_info::Location const& loc, size_t const len, uint8_t* const setme)
{
    auto error = tr_error{};
    read_or_write_piece(tor, false /*writable*/, loc, setme, len, error);
    return error.code();
}

int tr_ioWrite(tr_torrent& tor, tr_block_info::Location const& loc, size_t const len, uint8_t const* const writeme)
{
    auto error = tr_error{};
    read_or_write_piece(tor, true /*writable*/, loc, const_cast<uint8_t*>(writeme), len, error);

    // if IO failed, set torrent's error if not already set
    if (error && tor.error().error_type() != TR_STAT_LOCAL_ERROR)
    {
        tor.error().set_local_error(error.message());
        tr_torrentStop(&tor);
    }

    return error.code();
}

int tr_ioPrepareWrite(
    tr_torrent& tor,
    tr_block_info::Location const& loc,
    size_t const len,
    std::vector<tr_disk_write_worker::Chunk>& setme)
{
    setme.clear();

    auto const close_all = [&setme]()
    {
        for (auto& chunk : setme)
        {
            if (chunk.fd != TR_BAD_SYS_FILE)
            {
                tr_sys_file_close(chunk.fd);
            }
        }
        setme.clear();
    };

    if (loc.piece >= tor.piece_count())
    {
        return EINVAL;
    }

    auto error = tr_error{};
    auto [file_index, file_offset] = tor.file_offset(loc);
    auto& session = *tor.session;
    auto& open_files = session.openFiles();
    auto remaining = uint64_t{ len };

    while (remaining != 0U)
    {
        auto const bytes_this_pass = std::min(remaining, tor.file_size(file_index) - file_offset);

        // A zero-length file contributes nothing to write, but still advances
        // the walk -- mirroring read_or_write_bytes()'s early return.
        if (tor.file_size(file_index) != 0U && bytes_this_pass != 0U)
        {
            auto const fd = get_fd(session, open_files, tor, true /*writable*/, file_index, error);
            if (!fd || error)
            {
                close_all();
                return error.code() != 0 ? error.code() : EIO;
            }

            // Duplicate it: `open_files` is an LRU pool and may close its copy
            // while this write is still queued.
            auto const dup_fd = tr_sys_file_duplicate(*fd, &error);
            if (dup_fd == TR_BAD_SYS_FILE)
            {
                close_all();
                return error.code() != 0 ? error.code() : EIO;
            }

            setme.emplace_back(tr_disk_write_worker::Chunk{ dup_fd, file_offset, bytes_this_pass });
        }

        remaining -= bytes_this_pass;
        ++file_index;
        file_offset = 0U;

        if (remaining != 0U && file_index >= tor.file_count())
        {
            close_all();
            return EINVAL;
        }
    }

    return 0;
}

void tr_ioReportWriteError(tr_torrent& tor, int const error_code)
{
    if (error_code == 0)
    {
        return;
    }

    if (tor.error().error_type() != TR_STAT_LOCAL_ERROR)
    {
        tor.error().set_local_error(tr_strerror(error_code));
        tr_torrentStop(&tor);
    }
}

bool tr_ioTestPiece(tr_torrent const& tor, tr_piece_index_t const piece)
{
    auto const hash = recalculate_hash(tor, piece);
    return hash && *hash == tor.piece_hash(piece);
}
