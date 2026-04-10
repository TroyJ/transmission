// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef> // std::byte
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "libtransmission/crypto-utils.h"
#include "libtransmission/error.h"
#include "libtransmission/file.h"
#include "libtransmission/quark.h"
#include "libtransmission/relocate-worker.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-macros.h"
#include "libtransmission/utils.h"
#include "libtransmission/variant.h"

using namespace std::chrono_literals;
using namespace std::literals::string_literals;
using namespace std::literals::string_view_literals;

namespace
{
auto constexpr CopyChunkSize = size_t{ 1024U * 1024U };
auto constexpr ProgressSaveInterval = 64U * 1024U * 1024U;
auto constexpr ProgressUpdateInterval = 5s;

[[nodiscard]] constexpr auto is_active_state(tr_torrent_relocation_state const state) noexcept
{
    return state != TR_RELOC_NONE && state != TR_RELOC_ERROR;
}

[[nodiscard]] auto phase_to_string(tr_torrent_relocation_state const state)
{
    switch (state)
    {
    case TR_RELOC_NONE:
        return "none"sv;
    case TR_RELOC_QUEUED:
        return "queued"sv;
    case TR_RELOC_COPYING:
        return "copying"sv;
    case TR_RELOC_VERIFYING:
        return "verifying"sv;
    case TR_RELOC_RENAMING:
        return "renaming"sv;
    case TR_RELOC_DELETING_SOURCE:
        return "deleting_source"sv;
    case TR_RELOC_ERROR:
        return "error"sv;
    }

    return "error"sv;
}

[[nodiscard]] std::optional<tr_torrent_relocation_state> phase_from_string(std::string_view const phase) noexcept
{
    if (phase == "none"sv)
    {
        return TR_RELOC_NONE;
    }
    if (phase == "queued"sv)
    {
        return TR_RELOC_QUEUED;
    }
    if (phase == "copying"sv)
    {
        return TR_RELOC_COPYING;
    }
    if (phase == "verifying"sv)
    {
        return TR_RELOC_VERIFYING;
    }
    if (phase == "renaming"sv)
    {
        return TR_RELOC_RENAMING;
    }
    if (phase == "deleting_source"sv)
    {
        return TR_RELOC_DELETING_SOURCE;
    }
    if (phase == "error"sv)
    {
        return TR_RELOC_ERROR;
    }

    return {};
}

[[nodiscard]] auto temp_path(tr_relocate_worker::Snapshot const& snapshot, tr_file_index_t const file_index)
{
    return tr_pathbuf{
        snapshot.target_root,
        '/',
        snapshot.metainfo.file_subpath(file_index),
        ".trreloc."sv,
        snapshot.info_hash_string,
        ".tmp"sv,
    };
}

[[nodiscard]] auto final_path(tr_relocate_worker::Snapshot const& snapshot, tr_file_index_t const file_index)
{
    return tr_pathbuf{ snapshot.target_root, '/', snapshot.metainfo.file_subpath(file_index) };
}

[[nodiscard]] bool ensure_parent_dir_exists(std::string_view const path, tr_error* const error)
{
    auto parent = tr_pathbuf{ path };
    parent.popdir();
    return tr_sys_dir_create(parent, TR_SYS_DIR_CREATE_PARENTS, 0777, error);
}

[[nodiscard]] auto file_size_if_exists(std::string_view const path)
{
    if (auto const info = tr_sys_path_get_info(path, 0); info && info->isFile())
    {
        return std::optional<uint64_t>{ info->size };
    }

    return std::optional<uint64_t>{};
}

struct Journal
{
    tr_torrent_relocation_state phase = TR_RELOC_COPYING;
    uint64_t bytes_total = {};
    uint64_t bytes_copied = {};
    tr_file_index_t file_index = {};
    uint64_t file_offset = {};
    std::string source_root;
    std::string target_root;
    std::string previous_download_dir;
    std::string previous_incomplete_dir;
    std::string error;
};

[[nodiscard]] auto relocation_dir(std::string_view const journal_file)
{
    auto dir = tr_pathbuf{ journal_file };
    dir.popdir();
    return dir;
}

[[nodiscard]] std::optional<Journal> load_journal(tr_relocate_worker::Snapshot const& snapshot)
{
    auto top = tr_variant_serde::json().parse_file(snapshot.journal_file);
    if (!top)
    {
        return {};
    }

    auto const* const map = top->get_if<tr_variant::Map>();
    if (map == nullptr)
    {
        return {};
    }

    auto journal = Journal{};
    if (auto const phase = map->value_if<std::string_view>(tr_quark_new("phase"sv)); phase)
    {
        journal.phase = phase_from_string(*phase).value_or(TR_RELOC_COPYING);
    }
    if (auto const value = map->value_if<int64_t>(tr_quark_new("bytes_total"sv)); value)
    {
        journal.bytes_total = static_cast<uint64_t>(*value);
    }
    if (auto const value = map->value_if<int64_t>(tr_quark_new("bytes_copied"sv)); value)
    {
        journal.bytes_copied = static_cast<uint64_t>(*value);
    }
    if (auto const value = map->value_if<int64_t>(tr_quark_new("file_index"sv)); value)
    {
        journal.file_index = static_cast<tr_file_index_t>(*value);
    }
    if (auto const value = map->value_if<int64_t>(tr_quark_new("file_offset"sv)); value)
    {
        journal.file_offset = static_cast<uint64_t>(*value);
    }
    if (auto const value = map->value_if<std::string_view>(tr_quark_new("source_root"sv)); value)
    {
        journal.source_root = std::string{ *value };
    }
    if (auto const value = map->value_if<std::string_view>(tr_quark_new("target_root"sv)); value)
    {
        journal.target_root = std::string{ *value };
    }
    if (auto const value = map->value_if<std::string_view>(tr_quark_new("previous_download_dir"sv)); value)
    {
        journal.previous_download_dir = std::string{ *value };
    }
    if (auto const value = map->value_if<std::string_view>(tr_quark_new("previous_incomplete_dir"sv)); value)
    {
        journal.previous_incomplete_dir = std::string{ *value };
    }
    if (auto const value = map->value_if<std::string_view>(tr_quark_new("error"sv)); value)
    {
        journal.error = std::string{ *value };
    }

    return journal;
}

[[nodiscard]] bool save_journal(tr_relocate_worker::Snapshot const& snapshot, Journal const& journal, tr_error* const error)
{
    if (!tr_sys_dir_create(relocation_dir(snapshot.journal_file), TR_SYS_DIR_CREATE_PARENTS, 0777, error))
    {
        return false;
    }

    auto out = tr_variant::Map{};
    out.try_emplace(tr_quark_new("phase"sv), tr_variant::unmanaged_string(phase_to_string(journal.phase)));
    out.try_emplace(tr_quark_new("bytes_total"sv), journal.bytes_total);
    out.try_emplace(tr_quark_new("bytes_copied"sv), journal.bytes_copied);
    out.try_emplace(tr_quark_new("file_index"sv), static_cast<int64_t>(journal.file_index));
    out.try_emplace(tr_quark_new("file_offset"sv), static_cast<int64_t>(journal.file_offset));
    out.try_emplace(tr_quark_new("source_root"sv), journal.source_root);
    out.try_emplace(tr_quark_new("target_root"sv), journal.target_root);
    out.try_emplace(tr_quark_new("previous_download_dir"sv), journal.previous_download_dir);
    out.try_emplace(tr_quark_new("previous_incomplete_dir"sv), journal.previous_incomplete_dir);
    out.try_emplace(tr_quark_new("updated_at"sv), static_cast<int64_t>(tr_time()));
    out.try_emplace(tr_quark_new("error"sv), journal.error);

    return tr_variant_serde::json().to_file(std::move(out), snapshot.journal_file);
}

void remove_journal(tr_relocate_worker::Snapshot const& snapshot)
{
    tr_sys_path_remove(snapshot.journal_file, nullptr);
}

[[nodiscard]] auto initial_journal(tr_relocate_worker::Snapshot const& snapshot)
{
    auto journal = Journal{};
    journal.bytes_total = snapshot.metainfo.total_size();
    journal.source_root = snapshot.source_root;
    journal.target_root = snapshot.target_root;
    journal.previous_download_dir = snapshot.previous_download_dir;
    journal.previous_incomplete_dir = snapshot.previous_incomplete_dir;

    if (auto const loaded = load_journal(snapshot); loaded)
    {
        journal = *loaded;
        if (std::empty(journal.source_root))
        {
            journal.source_root = snapshot.source_root;
        }
        if (std::empty(journal.target_root))
        {
            journal.target_root = snapshot.target_root;
        }
        if (std::empty(journal.previous_download_dir))
        {
            journal.previous_download_dir = snapshot.previous_download_dir;
        }
        if (std::empty(journal.previous_incomplete_dir))
        {
            journal.previous_incomplete_dir = snapshot.previous_incomplete_dir;
        }
        if (journal.bytes_total == 0U)
        {
            journal.bytes_total = snapshot.metainfo.total_size();
        }
    }

    return journal;
}

[[nodiscard]] auto source_path(tr_relocate_worker::Snapshot const& snapshot, Journal const& journal, tr_file_index_t const file_index)
{
    return tr_pathbuf{ journal.source_root, '/', snapshot.metainfo.file_subpath(file_index) };
}

[[nodiscard]] auto existing_completed_bytes(tr_relocate_worker::Snapshot const& snapshot)
{
    auto bytes = uint64_t{};
    for (tr_file_index_t file_index = 0, n_files = snapshot.metainfo.file_count(); file_index < n_files; ++file_index)
    {
        auto const file_size = snapshot.metainfo.file_size(file_index);
        if (file_size == 0U)
        {
            continue;
        }

        if (auto const size = file_size_if_exists(final_path(snapshot, file_index)); size && *size == file_size)
        {
            bytes += file_size;
            continue;
        }

        if (auto const size = file_size_if_exists(temp_path(snapshot, file_index)); size)
        {
            bytes += std::min(*size, file_size);
        }
    }

    return bytes;
}

[[nodiscard]] bool all_final_files_ready(tr_relocate_worker::Snapshot const& snapshot)
{
    for (tr_file_index_t file_index = 0, n_files = snapshot.metainfo.file_count(); file_index < n_files; ++file_index)
    {
        auto const file_size = snapshot.metainfo.file_size(file_index);
        auto const final = final_path(snapshot, file_index);
        if (file_size == 0U)
        {
            if (auto const info = tr_sys_path_get_info(final, 0); info && info->isFile())
            {
                continue;
            }

            return false;
        }

        if (auto const size = file_size_if_exists(final); !size || *size != file_size)
        {
            return false;
        }
    }

    return true;
}

[[nodiscard]] bool all_temp_files_ready(tr_relocate_worker::Snapshot const& snapshot)
{
    for (tr_file_index_t file_index = 0, n_files = snapshot.metainfo.file_count(); file_index < n_files; ++file_index)
    {
        auto const file_size = snapshot.metainfo.file_size(file_index);
        if (file_size == 0U)
        {
            continue;
        }

        if (auto const size = file_size_if_exists(temp_path(snapshot, file_index)); !size || *size != file_size)
        {
            return false;
        }
    }

    return true;
}

[[nodiscard]] bool copy_file(
    tr_relocate_worker::Snapshot const& snapshot,
    Journal& journal,
    tr_relocate_worker::Mediator& mediator,
    std::atomic<bool> const& abort_flag,
    tr_file_index_t const file_index,
    std::vector<std::byte>& buffer,
    std::chrono::steady_clock::time_point& last_progress_at,
    uint64_t& bytes_since_last_save,
    uint64_t& bytes_since_last_rate,
    std::chrono::steady_clock::time_point& last_rate_at,
    tr_error* const error)
{
    auto const file_size = snapshot.metainfo.file_size(file_index);
    auto const src = source_path(snapshot, journal, file_index);
    auto const dst = temp_path(snapshot, file_index);

    if (!ensure_parent_dir_exists(dst, error))
    {
        return false;
    }

    auto offset = uint64_t{};
    if (auto const size = file_size_if_exists(final_path(snapshot, file_index)); size && *size == file_size)
    {
        journal.file_index = file_index + 1U;
        journal.file_offset = 0U;
        return true;
    }

    if (auto const existing = file_size_if_exists(dst); existing && *existing <= file_size)
    {
        offset = *existing;
    }
    else if (auto const existing = file_size_if_exists(dst); existing && *existing > file_size)
    {
        tr_sys_path_remove(dst, nullptr);
    }

    if (file_size == 0U)
    {
        auto const fd = tr_sys_file_open(dst, TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE | TR_SYS_FILE_TRUNCATE, 0666, error);
        if (fd == TR_BAD_SYS_FILE)
        {
            return false;
        }

        if (!tr_sys_file_close(fd, error))
        {
            return false;
        }

        journal.file_index = file_index + 1U;
        journal.file_offset = 0U;
        return true;
    }

    auto const in = tr_sys_file_open(src, TR_SYS_FILE_READ | TR_SYS_FILE_SEQUENTIAL, 0, error);
    if (in == TR_BAD_SYS_FILE)
    {
        return false;
    }

    auto const out = tr_sys_file_open(dst, TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE, 0666, error);
    if (out == TR_BAD_SYS_FILE)
    {
        tr_sys_file_close(in);
        return false;
    }

    while (offset < file_size)
    {
        if (abort_flag)
        {
            tr_sys_file_close(out, nullptr);
            tr_sys_file_close(in, nullptr);
            return false;
        }

        auto const this_pass = std::min<uint64_t>(file_size - offset, std::size(buffer));
        auto bytes_read = uint64_t{};
        if (!tr_sys_file_read_at(in, std::data(buffer), this_pass, offset, &bytes_read, error) || bytes_read == 0U)
        {
            if (error != nullptr && !*error)
            {
                error->set(
                    EIO,
                    fmt::format(
                        "Couldn't read '{}' at {} bytes",
                        src,
                        offset));
            }
            tr_sys_file_close(out, nullptr);
            tr_sys_file_close(in, nullptr);
            return false;
        }

        auto wrote_total = uint64_t{};
        while (wrote_total < bytes_read)
        {
            auto bytes_written = uint64_t{};
            if (!tr_sys_file_write_at(
                    out,
                    std::data(buffer) + wrote_total,
                    bytes_read - wrote_total,
                    offset + wrote_total,
                    &bytes_written,
                    error) ||
                bytes_written == 0U)
            {
                if (error != nullptr && !*error)
                {
                    error->set(
                        EIO,
                        fmt::format(
                            "Couldn't write '{}' at {} bytes",
                            dst,
                            offset + wrote_total));
                }
                tr_sys_file_close(out, nullptr);
                tr_sys_file_close(in, nullptr);
                return false;
            }

            wrote_total += bytes_written;
        }

        offset += bytes_read;
        journal.file_index = file_index;
        journal.file_offset = offset;
        journal.bytes_copied += bytes_read;
        bytes_since_last_save += bytes_read;
        bytes_since_last_rate += bytes_read;

        auto const now = std::chrono::steady_clock::now();
        auto const save_needed = bytes_since_last_save >= ProgressSaveInterval || now - last_progress_at >= ProgressUpdateInterval;
        if (save_needed)
        {
            auto rate_bps = uint64_t{};
            if (auto const elapsed = now - last_rate_at; elapsed > 0ms)
            {
                rate_bps = static_cast<uint64_t>(bytes_since_last_rate / std::chrono::duration<double>(elapsed).count());
            }

            if (!save_journal(snapshot, journal, error))
            {
                tr_sys_file_close(out, nullptr);
                tr_sys_file_close(in, nullptr);
                return false;
            }

            mediator.on_relocate_state_changed(TR_RELOC_COPYING, journal.bytes_copied, journal.bytes_total, rate_bps, {});
            bytes_since_last_save = 0U;
            bytes_since_last_rate = 0U;
            last_progress_at = now;
            last_rate_at = now;
        }
    }

    if (!tr_sys_file_truncate(out, file_size, error) || !tr_sys_file_close(out, error) || !tr_sys_file_close(in, error))
    {
        return false;
    }

    journal.file_index = file_index + 1U;
    journal.file_offset = 0U;
    return true;
}

[[nodiscard]] bool copy_files(
    tr_relocate_worker::Snapshot const& snapshot,
    Journal& journal,
    tr_relocate_worker::Mediator& mediator,
    std::atomic<bool> const& abort_flag,
    tr_error* const error)
{
    journal.bytes_total = snapshot.metainfo.total_size();
    journal.phase = TR_RELOC_COPYING;
    journal.bytes_copied = existing_completed_bytes(snapshot);
    journal.file_index = 0U;
    journal.file_offset = 0U;

    if (!save_journal(snapshot, journal, error))
    {
        return false;
    }

    mediator.on_relocate_state_changed(TR_RELOC_COPYING, journal.bytes_copied, journal.bytes_total, 0U, {});

    auto buffer = std::vector<std::byte>(CopyChunkSize);
    auto last_progress_at = std::chrono::steady_clock::now();
    auto last_rate_at = last_progress_at;
    auto bytes_since_last_save = uint64_t{};
    auto bytes_since_last_rate = uint64_t{};

    for (tr_file_index_t file_index = 0, n_files = snapshot.metainfo.file_count(); file_index < n_files; ++file_index)
    {
        if (!copy_file(
                snapshot,
                journal,
                mediator,
                abort_flag,
                file_index,
                buffer,
                last_progress_at,
                bytes_since_last_save,
                bytes_since_last_rate,
                last_rate_at,
                error))
        {
            return false;
        }
    }

    journal.bytes_copied = journal.bytes_total;
    journal.file_index = snapshot.metainfo.file_count();
    journal.file_offset = 0U;
    if (!save_journal(snapshot, journal, error))
    {
        return false;
    }

    mediator.on_relocate_state_changed(TR_RELOC_COPYING, journal.bytes_copied, journal.bytes_total, 0U, {});
    return true;
}

[[nodiscard]] auto relocation_verify_path(tr_relocate_worker::Snapshot const& snapshot, tr_file_index_t const file_index)
{
    auto const final = final_path(snapshot, file_index);
    if (tr_sys_path_exists(final))
    {
        return std::optional<std::string>{ std::string{ final } };
    }

    auto const tmp = temp_path(snapshot, file_index);
    if (tr_sys_path_exists(tmp))
    {
        return std::optional<std::string>{ std::string{ tmp } };
    }

    return std::optional<std::string>{};
}

[[nodiscard]] bool verify_files(
    tr_relocate_worker::Snapshot const& snapshot,
    Journal& journal,
    tr_relocate_worker::Mediator& mediator,
    std::atomic<bool> const& abort_flag,
    tr_error* const error)
{
    journal.phase = TR_RELOC_VERIFYING;
    journal.bytes_copied = journal.bytes_total;
    journal.file_index = 0U;
    journal.file_offset = 0U;
    if (!save_journal(snapshot, journal, error))
    {
        return false;
    }

    mediator.on_relocate_state_changed(TR_RELOC_VERIFYING, journal.bytes_copied, journal.bytes_total, 0U, {});

    tr_sys_file_t fd = TR_BAD_SYS_FILE;
    auto file_pos = uint64_t{};
    auto piece_pos = uint32_t{};
    auto file_index = tr_file_index_t{};
    auto prev_file_index = tr_file_index_t{ ~0U };
    auto piece = tr_piece_index_t{};
    auto buffer = std::vector<std::byte>(1024U * 256U);
    auto sha = tr_sha1{};

    auto const& metainfo = snapshot.metainfo;
    while (!abort_flag && piece < metainfo.piece_count())
    {
        auto const file_length = metainfo.file_size(file_index);

        if (file_pos == 0U && fd == TR_BAD_SYS_FILE && file_index != prev_file_index)
        {
            if (auto const found = relocation_verify_path(snapshot, file_index); found)
            {
                fd = tr_sys_file_open(found->c_str(), TR_SYS_FILE_READ | TR_SYS_FILE_SEQUENTIAL, 0, error);
            }
            else
            {
                fd = TR_BAD_SYS_FILE;
            }
            prev_file_index = file_index;
        }

        auto left_in_piece = metainfo.piece_size(piece) - piece_pos;
        auto left_in_file = file_length - file_pos;
        auto bytes_this_pass = std::min<uint64_t>(left_in_file, left_in_piece);
        bytes_this_pass = std::min<uint64_t>(bytes_this_pass, std::size(buffer));

        if (fd != TR_BAD_SYS_FILE && bytes_this_pass > 0U)
        {
            auto num_read = uint64_t{};
            if (tr_sys_file_read_at(fd, std::data(buffer), bytes_this_pass, file_pos, &num_read, error) && num_read > 0U)
            {
                bytes_this_pass = num_read;
                sha.add(std::data(buffer), bytes_this_pass);
            }
            else
            {
                bytes_this_pass = 0U;
            }
        }

        left_in_piece -= bytes_this_pass;
        left_in_file -= bytes_this_pass;
        piece_pos += bytes_this_pass;
        file_pos += bytes_this_pass;

        if (left_in_piece == 0U)
        {
            if (sha.finish() != metainfo.piece_hash(piece))
            {
                if (fd != TR_BAD_SYS_FILE)
                {
                    tr_sys_file_close(fd, nullptr);
                }
                if (error != nullptr && !*error)
                {
                    error->set(EIO, fmt::format("Verification failed for piece {}", piece));
                }
                return false;
            }

            sha.clear();
            ++piece;
            piece_pos = 0U;
        }

        if (left_in_file == 0U)
        {
            if (fd != TR_BAD_SYS_FILE)
            {
                tr_sys_file_close(fd, nullptr);
                fd = TR_BAD_SYS_FILE;
            }

            ++file_index;
            file_pos = 0U;
        }
    }

    if (fd != TR_BAD_SYS_FILE)
    {
        tr_sys_file_close(fd, nullptr);
    }

    if (abort_flag)
    {
        return false;
    }

    return piece == metainfo.piece_count();
}

[[nodiscard]] bool rename_temp_files(
    tr_relocate_worker::Snapshot const& snapshot,
    Journal& journal,
    tr_relocate_worker::Mediator& mediator,
    std::atomic<bool> const& abort_flag,
    tr_error* const error)
{
    journal.phase = TR_RELOC_RENAMING;
    if (!save_journal(snapshot, journal, error))
    {
        return false;
    }

    mediator.on_relocate_state_changed(TR_RELOC_RENAMING, journal.bytes_total, journal.bytes_total, 0U, {});

    for (tr_file_index_t file_index = 0, n_files = snapshot.metainfo.file_count(); file_index < n_files; ++file_index)
    {
        if (abort_flag)
        {
            return false;
        }

        auto const file_size = snapshot.metainfo.file_size(file_index);
        auto const final = final_path(snapshot, file_index);
        auto const tmp = temp_path(snapshot, file_index);

        if (file_size == 0U)
        {
            if (tr_sys_path_exists(final))
            {
                continue;
            }

            auto const fd = tr_sys_file_open(final, TR_SYS_FILE_WRITE | TR_SYS_FILE_CREATE | TR_SYS_FILE_TRUNCATE, 0666, error);
            if (fd == TR_BAD_SYS_FILE)
            {
                return false;
            }
            if (!tr_sys_file_close(fd, error))
            {
                return false;
            }
            continue;
        }

        if (auto const size = file_size_if_exists(final); size && *size == file_size)
        {
            continue;
        }

        if (!tr_sys_path_exists(tmp))
        {
            if (error != nullptr)
            {
                error->set(ENOENT, fmt::format("Missing relocation temporary file '{}'", tmp));
            }
            return false;
        }

        if (tr_sys_path_exists(final))
        {
            if (error != nullptr)
            {
                error->set(EEXIST, fmt::format("Destination file already exists '{}'", final));
            }
            return false;
        }

        if (!tr_sys_path_rename(tmp, final, error))
        {
            return false;
        }
    }

    return true;
}

[[nodiscard]] bool delete_source(
    tr_relocate_worker::Snapshot const& snapshot,
    Journal& journal,
    tr_relocate_worker::Mediator& mediator,
    std::atomic<bool> const& abort_flag,
    tr_error* const error)
{
    journal.phase = TR_RELOC_DELETING_SOURCE;
    if (!save_journal(snapshot, journal, error))
    {
        return false;
    }

    mediator.on_relocate_state_changed(TR_RELOC_DELETING_SOURCE, journal.bytes_total, journal.bytes_total, 0U, {});
    if (abort_flag)
    {
        return false;
    }

    auto const delete_func = [](char const* filename)
    {
        tr_sys_path_remove(filename, nullptr);
    };

    snapshot.metainfo.files().remove(journal.source_root, snapshot.name, delete_func, error);
    return !error || !*error;
}
} // namespace

int tr_relocate_worker::Node::compare(Node const& that) const noexcept
{
    if (priority_ != that.priority_)
    {
        return priority_ > that.priority_ ? -1 : 1;
    }

    auto const this_size = mediator_->snapshot().metainfo.total_size();
    auto const that_size = that.mediator_->snapshot().metainfo.total_size();
    if (this_size != that_size)
    {
        return this_size < that_size ? -1 : 1;
    }

    auto const& this_hash = mediator_->snapshot().info_hash;
    auto const& that_hash = that.mediator_->snapshot().info_hash;
    if (this_hash != that_hash)
    {
        return this_hash < that_hash ? -1 : 1;
    }

    return 0;
}

bool tr_relocate_worker::add(std::unique_ptr<Mediator> mediator, tr_priority_t const priority)
{
    auto const lock = std::scoped_lock{ relocate_mutex_ };
    auto const& snapshot = mediator->snapshot();

    if ((current_node_ && current_node_->matches(snapshot.info_hash)) ||
        std::any_of(std::begin(todo_), std::end(todo_), [&snapshot](auto const& node) { return node.matches(snapshot.info_hash); }))
    {
        return false;
    }

    mediator->on_relocate_state_changed(TR_RELOC_QUEUED, 0U, snapshot.metainfo.total_size(), 0U, {});
    todo_.emplace(std::move(mediator), priority);

    if (!relocate_thread_id_)
    {
        auto thread = std::thread(&tr_relocate_worker::relocate_thread_func, this);
        relocate_thread_id_ = thread.get_id();
        thread.detach();
    }

    return true;
}

void tr_relocate_worker::remove(tr_sha1_digest_t const& info_hash)
{
    auto lock = std::unique_lock{ relocate_mutex_ };

    if (current_node_ && current_node_->matches(info_hash))
    {
        stop_current_ = true;
        stop_current_cv_.wait(lock, [this]() { return !stop_current_; });
    }
    else if (auto const iter = std::find_if(
                 std::begin(todo_),
                 std::end(todo_),
                 [&info_hash](auto const& node) { return node.matches(info_hash); });
             iter != std::end(todo_))
    {
        todo_.erase(iter);
    }
}

tr_relocate_worker::~tr_relocate_worker()
{
    {
        auto const lock = std::scoped_lock{ relocate_mutex_ };
        stop_current_ = true;
        todo_.clear();
    }

    while (relocate_thread_id_.has_value())
    {
        std::this_thread::sleep_for(20ms);
    }
}

void tr_relocate_worker::relocate_thread_func()
{
    while (true)
    {
        {
            auto const lock = std::scoped_lock{ relocate_mutex_ };
            if (todo_.empty())
            {
                relocate_thread_id_.reset();
                break;
            }

            current_node_ = std::move(todo_.extract(std::begin(todo_)).value());
        }

        auto& mediator = *current_node_->mediator_;
        auto const snapshot = mediator.snapshot();
        auto journal = initial_journal(snapshot);
        auto error = tr_error{};
        auto aborted = false;

        auto const finish_current = [this, &aborted]()
        {
            auto lock = std::unique_lock{ relocate_mutex_ };
            aborted = stop_current_;
            current_node_.reset();
            stop_current_ = false;
            lock.unlock();
            stop_current_cv_.notify_all();
        };

        if (journal.phase == TR_RELOC_DELETING_SOURCE && all_final_files_ready(snapshot))
        {
            if (!mediator.on_verified_location_ready())
            {
                finish_current();
                continue;
            }

            if (!delete_source(snapshot, journal, mediator, stop_current_, &error))
            {
                if (!stop_current_)
                {
                    journal.phase = TR_RELOC_ERROR;
                    journal.error = error ? error.message() : "Relocation delete failed"s;
                    (void)save_journal(snapshot, journal, nullptr);
                    mediator.on_relocate_state_changed(
                        TR_RELOC_ERROR,
                        journal.bytes_total,
                        journal.bytes_total,
                        0U,
                        journal.error);
                }

                finish_current();
                continue;
            }

            mediator.on_source_deleted();
            remove_journal(snapshot);
            mediator.on_relocate_state_changed(TR_RELOC_NONE, journal.bytes_total, journal.bytes_total, 0U, {});
            finish_current();
            continue;
        }

        auto ok = copy_files(snapshot, journal, mediator, stop_current_, &error);
        if (ok)
        {
            ok = verify_files(snapshot, journal, mediator, stop_current_, &error);
        }
        if (ok)
        {
            ok = rename_temp_files(snapshot, journal, mediator, stop_current_, &error);
        }
        if (ok)
        {
            ok = mediator.on_verified_location_ready();
        }
        if (ok)
        {
            ok = delete_source(snapshot, journal, mediator, stop_current_, &error);
        }

        if (ok)
        {
            mediator.on_source_deleted();
            remove_journal(snapshot);
            mediator.on_relocate_state_changed(TR_RELOC_NONE, journal.bytes_total, journal.bytes_total, 0U, {});
        }
        else if (!stop_current_)
        {
            journal.phase = TR_RELOC_ERROR;
            journal.error = error ? error.message() : "Relocation failed"s;
            (void)save_journal(snapshot, journal, nullptr);
            mediator.on_relocate_state_changed(TR_RELOC_ERROR, journal.bytes_copied, journal.bytes_total, 0U, journal.error);
        }

        finish_current();
    }
}
