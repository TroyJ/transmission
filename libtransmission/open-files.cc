// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm> // std::min
#include <array>
#include <cstdint> // uint8_t, uint64_t
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "libtransmission/transmission.h"

#include "libtransmission/error-types.h"
#include "libtransmission/error.h"
#include "libtransmission/file.h"
#include "libtransmission/log.h"
#include "libtransmission/open-files.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-strbuf.h"
#include "libtransmission/utils.h" // _()

namespace
{

[[nodiscard]] auto is_open(tr_sys_file_t fd) noexcept
{
    return fd != TR_BAD_SYS_FILE;
}

bool preallocate_file_sparse(tr_sys_file_t fd, uint64_t length, tr_error* error)
{
    if (length == 0U)
    {
        return true;
    }

    auto local_error = tr_error{};

    if (tr_sys_file_preallocate(fd, length, TR_SYS_FILE_PREALLOC_SPARSE, &local_error))
    {
        return true;
    }

    tr_logAddDebug(fmt::format("Fast preallocation failed: {} ({})", local_error.message(), local_error.code()));

    // A full disk will not get better by being asked a second way.
    if (tr_error_is_enospc(local_error.code()))
    {
        if (error != nullptr)
        {
            *error = std::move(local_error);
        }

        return false;
    }

    // Deliberately no fallback.
    //
    // This used to seek-and-write -- write one byte at `length - 1`, then
    // ftruncate -- so the file would appear full-size right away. That is free
    // only on a filesystem with sparse-file support. On one without it, notably
    // exFAT (the usual choice for a portable drive shared with Windows), it
    // physically allocates and zero-fills the entire file. That happens here,
    // inside tr_open_files::get(), on the session thread, under the session
    // lock, so starting a large file froze the GUI and the RPC server for as
    // long as the zero-fill took: measured at ~7s per 512 MiB and ~24s per GiB
    // on an exFAT/FSKit volume, i.e. minutes for a large file.
    //
    // Sparse preallocation exists to be free; a fallback that costs a full-file
    // write defeats its own purpose. When the cheap path is unavailable we do
    // nothing instead. The file is still created, and still grows as blocks are
    // written to it -- exactly what happens with preallocation turned off, which
    // is already a supported configuration.
    //
    // See docs/disk-stall-investigation.md.
    return true;
}

bool preallocate_file_full(tr_sys_file_t fd, uint64_t length, tr_error* error)
{
    if (length == 0U)
    {
        return true;
    }

    auto local_error = tr_error{};

    if (tr_sys_file_preallocate(fd, length, 0, &local_error))
    {
        return true;
    }

    tr_logAddDebug(fmt::format("Full preallocation failed: {} ({})", local_error.message(), local_error.code()));

    if (!tr_error_is_enospc(local_error.code()))
    {
        auto buf = std::array<uint8_t, 4096>{};
        bool success = true;

        local_error = {};

        /* fallback: the old-fashioned way */
        while (success && length > 0)
        {
            uint64_t const this_pass = std::min(length, uint64_t{ std::size(buf) });
            uint64_t bytes_written = 0;
            success = tr_sys_file_write(fd, std::data(buf), this_pass, &bytes_written, &local_error);
            length -= bytes_written;
        }

        if (success)
        {
            return true;
        }

        tr_logAddDebug(fmt::format("Full preallocation fallback failed: {} ({})", local_error.message(), local_error.code()));
    }

    if (error != nullptr)
    {
        *error = std::move(local_error);
    }

    return false;
}

} // unnamed namespace

// ---

tr_sys_file_t tr_open_files::open_file(
    std::string_view const filename_in,
    bool writable,
    Preallocation const allocation,
    uint64_t const file_size,
    tr_error* const error_out)
{
    // create subfolders, if any
    auto const filename = tr_pathbuf{ filename_in };
    auto error = tr_error{};
    if (writable)
    {
        auto dir = tr_pathbuf{ filename.sv() };
        dir.popdir();
        if (!tr_sys_dir_create(dir, TR_SYS_DIR_CREATE_PARENTS, 0777, &error))
        {
            tr_logAddError(
                fmt::format(
                    fmt::runtime(_("Couldn't create '{path}': {error} ({error_code})")),
                    fmt::arg("path", dir),
                    fmt::arg("error", error.message()),
                    fmt::arg("error_code", error.code())));
            if (error_out != nullptr)
            {
                *error_out = std::move(error);
            }
            return TR_BAD_SYS_FILE;
        }
    }

    auto const info = tr_sys_path_get_info(filename);
    bool const already_existed = info && info->isFile();

    // we need write permissions to resize the file
    bool const resize_needed = already_existed && (file_size < info->size);
    writable |= resize_needed;

    // open the file
    int flags = writable ? (TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE) : 0;
    flags |= TR_SYS_FILE_READ;
    auto const fd = tr_sys_file_open(filename, flags, 0666, &error);
    if (!is_open(fd))
    {
        tr_logAddError(
            fmt::format(
                fmt::runtime(_("Couldn't open '{path}': {error} ({error_code})")),
                fmt::arg("path", filename),
                fmt::arg("error", error.message()),
                fmt::arg("error_code", error.code())));
        if (error_out != nullptr)
        {
            *error_out = std::move(error);
        }
        return TR_BAD_SYS_FILE;
    }

    if (writable && !already_existed && allocation != Preallocation::None)
    {
        bool success = false;
        char const* type = nullptr;

        if (allocation == Preallocation::Full)
        {
            success = preallocate_file_full(fd, file_size, &error);
            type = "full";
        }
        else if (allocation == Preallocation::Sparse)
        {
            success = preallocate_file_sparse(fd, file_size, &error);
            type = "sparse";
        }

        TR_ASSERT(type != nullptr);

        if (!success)
        {
            tr_logAddError(
                fmt::format(
                    fmt::runtime(_("Couldn't preallocate '{path}': {error} ({error_code})")),
                    fmt::arg("path", filename),
                    fmt::arg("error", error.message()),
                    fmt::arg("error_code", error.code())));
            tr_sys_file_close(fd);
            if (error_out != nullptr)
            {
                *error_out = std::move(error);
            }
            return TR_BAD_SYS_FILE;
        }

        tr_logAddDebug(fmt::format("Preallocated file '{}' ({}, size: {})", filename, type, file_size));
    }

    // If the file already exists and it's too large, truncate it.
    // This is a fringe case that happens if a torrent's been updated
    // and one of the updated torrent's files is smaller.
    // https://trac.transmissionbt.com/ticket/2228
    // https://bugs.launchpad.net/ubuntu/+source/transmission/+bug/318249
    if (resize_needed && !tr_sys_file_truncate(fd, file_size, &error))
    {
        tr_logAddWarn(
            fmt::format(
                fmt::runtime(_("Couldn't truncate '{path}': {error} ({error_code})")),
                fmt::arg("path", filename),
                fmt::arg("error", error.message()),
                fmt::arg("error_code", error.code())));
        tr_sys_file_close(fd);
        if (error_out != nullptr)
        {
            *error_out = std::move(error);
        }
        return TR_BAD_SYS_FILE;
    }

    return fd;
}

std::optional<tr_sys_file_t> tr_open_files::get(tr_torrent_id_t tor_id, tr_file_index_t file_num, bool writable)
{
    if (auto* const found = pool_.get(make_key(tor_id, file_num)); found != nullptr)
    {
        if (writable && !found->writable_)
        {
            return {};
        }

        return found->fd_;
    }

    return {};
}

std::optional<tr_sys_file_t> tr_open_files::get(
    tr_torrent_id_t tor_id,
    tr_file_index_t file_num,
    bool writable,
    std::string_view filename_in,
    Preallocation allocation,
    uint64_t file_size)
{
    // is there already an entry
    auto key = make_key(tor_id, file_num);
    if (auto* const found = pool_.get(key); found != nullptr)
    {
        if (!writable || found->writable_)
        {
            return found->fd_;
        }

        pool_.erase(key); // close so we can re-open as writable
    }

    auto error = tr_error{};
    auto const fd = open_file(filename_in, writable, allocation, file_size, &error);
    if (!is_open(fd))
    {
        return {};
    }

    // cache it
    auto& entry = pool_.add(std::move(key));
    entry.fd_ = fd;
    entry.writable_ = writable;
    entry.owner_ = this;

    return fd;
}

void tr_open_files::close_all()
{
    pool_.clear();
}

void tr_open_files::close_torrent(tr_torrent_id_t tor_id)
{
    pool_.erase_if([&tor_id](Key const& key, Val const& /*unused*/) { return key.first == tor_id; });
}

void tr_open_files::close_file(tr_torrent_id_t tor_id, tr_file_index_t file_num)
{
    pool_.erase(make_key(tor_id, file_num));
}

void tr_open_files::set_close_handler(std::function<void(tr_sys_file_t)> handler)
{
    close_handler_ = std::move(handler);
}

tr_open_files::Val::~Val()
{
    if (!is_open(fd_))
    {
        return;
    }

    // close() blocks on a slow disk; hand it off if someone is willing to take it
    if (owner_ != nullptr && owner_->close_handler_)
    {
        owner_->close_handler_(fd_);
        return;
    }

    tr_sys_file_close(fd_);
}
