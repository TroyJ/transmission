// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#ifndef __TRANSMISSION__
#error only libtransmission should #include this header.
#endif

#include <condition_variable>
#include <cstddef> // std::byte
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "libtransmission/transmission.h"

#include "libtransmission/crypto-utils.h" // tr_sha1_digest_t

/**
 * Hashes single pieces on a background thread so that the session
 * mutex is never held across the disk reads.
 *
 * A job is a self-contained snapshot: file paths + offsets for the
 * piece's bytes, plus copies of any blocks that were still sitting
 * in the write cache when the snapshot was taken. The worker never
 * touches tr_session or tr_torrent state; the caller's `on_done`
 * callback is responsible for re-entering the session thread and
 * revalidating before committing the result.
 */
class tr_piece_check_worker
{
public:
    enum class Result : uint8_t
    {
        Pass, // the data hashed to the expected digest
        Fail, // the data was readable but did not match
        Unreadable // a file in the snapshot could not be opened
    };

    struct Span
    {
        tr_file_index_t file_index = 0U;
        std::string path; // empty if not yet resolved (see `candidates`) or the file does not exist
        // Where the file may be when `path` is empty, in search order. Resolving
        // a path is a stat on the data volume, which must not happen on the
        // session thread (that is the 4 s hold at peer-io.cc:367, live on the
        // SD card 2026-08-26); the reader tries these off the lock instead and
        // reports which one it opened so the torrent can remember it.
        std::vector<std::string> candidates;
        uint64_t file_offset = 0U;
        uint64_t length = 0U;
    };

    using ResolvedPath = std::pair<tr_file_index_t, std::string>;

    struct CachedBytes
    {
        uint64_t piece_offset = 0U;
        std::vector<uint8_t> data;
    };

    struct Job
    {
        tr_sha1_digest_t expected_hash = {};
        uint64_t piece_size = 0U;
        std::vector<Span> spans; // in piece order; lengths sum to piece_size
        std::vector<CachedBytes> cached; // overlays applied after the disk reads
        std::function<void(Result)> on_done; // invoked on the worker thread
    };

    tr_piece_check_worker() = default;
    ~tr_piece_check_worker();

    tr_piece_check_worker(tr_piece_check_worker const&) = delete;
    tr_piece_check_worker(tr_piece_check_worker&&) = delete;
    tr_piece_check_worker& operator=(tr_piece_check_worker const&) = delete;
    tr_piece_check_worker& operator=(tr_piece_check_worker&&) = delete;

    void add(Job&& job);

    // Run an arbitrary task on the worker thread. Same rules as hash jobs:
    // the task must not touch tr_session / tr_torrent state; post back to
    // the session thread to commit its result.
    void run(std::function<void()> task);

    [[nodiscard]] static Result hash_job(Job const& job, std::vector<std::byte>& buffer);

    // Reads the spans, in order, into `buffer` (which must hold their total
    // length). Short reads leave the tail as-is. Returns false if any file
    // could not be opened.
    // ...and, if `resolved` is given, which candidate each unresolved span opened.
    [[nodiscard]] static bool read_spans(
        std::vector<Span> const& spans,
        std::byte* buffer,
        std::vector<ResolvedPath>* resolved = nullptr);

private:
    void thread_func();

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> todo_;
    std::thread thread_;
    bool stopping_ = false;
};
