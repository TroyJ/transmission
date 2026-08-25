// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <cstddef> // size_t
#include <cstdint> // uint8_t, uint32_t
#include <vector>

#include "libtransmission/transmission.h"

#include "libtransmission/block-info.h"
#include "libtransmission/disk-write-worker.h"

struct tr_torrent;

/**
 * @addtogroup file_io File IO
 * @{
 */

/**
 * Reads the block specified by the piece index, offset, and length.
 * @return 0 on success, or an errno value on failure.
 */
[[nodiscard]] int tr_ioRead(tr_torrent const& tor, tr_block_info::Location const& loc, size_t len, uint8_t* setme);

/**
 * Writes the block specified by the piece index, offset, and length.
 * @return 0 on success, or an errno value on failure.
 */
[[nodiscard]] int tr_ioWrite(tr_torrent& tor, tr_block_info::Location const& loc, size_t len, uint8_t const* writeme);

/**
 * Resolves the files a write would land in and opens them, without writing.
 *
 * This is the half of the write path that must stay on the session thread:
 * it touches tr_torrent's file layout and the session's `tr_open_files` pool,
 * neither of which is thread-safe. It is also the cheap half -- opens measure
 * in microseconds where the writes they precede have been measured in minutes.
 *
 * The returned chunks hold **duplicated** descriptors, so the fd pool may evict
 * and close its own copies while the write is still in flight. Ownership passes
 * to the caller, which must hand them to tr_disk_write_worker (it closes them)
 * or close them itself.
 *
 * @return 0 on success, or an errno value on failure. On failure `setme` is
 *         emptied and any descriptors already opened are closed.
 */
[[nodiscard]] int tr_ioPrepareWrite(
    tr_torrent& tor,
    tr_block_info::Location const& loc,
    size_t len,
    std::vector<tr_disk_write_worker::Chunk>& setme);

/**
 * Records a write failure against the torrent: sets its local error and stops
 * it, exactly as the old inline write path did. Must be called on the session
 * thread; the async write path posts back here once a job reports an error.
 */
void tr_ioReportWriteError(tr_torrent& tor, int error_code);

/**
 * @brief Test to see if the piece matches its metainfo's SHA1 checksum.
 */
[[nodiscard]] bool tr_ioTestPiece(tr_torrent const& tor, tr_piece_index_t piece);

/* @} */
