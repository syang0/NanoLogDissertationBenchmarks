/* Copyright (c) 2016-2019 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <fcntl.h>
#include <cassert>
#include <cstdint>

/**
 * This file centralizes the Configuration Options that can be made to NanoLog
 * and the benchmarking application
 */

namespace NanoLogConfig {

    // Represents the number of data items across all threads that will be
    // push-ed() into the test StagingBuffer
    constexpr long ITERATIONS = 1000000;

    // Number of threads that will push() data into into the StagingBuffer.
    constexpr int BENCHMARK_THREADS = 2;

    // The datum that will be push()-ed into the StagingBuffer
    constexpr const char *datum = "123456789012345";

    // Statically computed size of the datum above
    constexpr size_t datum_len = strlen(datum) + 1;

    // Determines the byte size of the per-thread StagingBuffer that decouples
    // the producer logging thread from the consumer background compression
    // thread. This value should be large enough to handle bursts of activity.
    static const uint32_t STAGING_BUFFER_SIZE = 1<<20;


    /***
     * Below are options that exist in real NanoLog but are unused in this repo.
     * If you use any of the variables below; please move them above this line.
     */

    // Controls in what mode the compressed log file will be opened
    static const int FILE_PARAMS = O_APPEND|O_RDWR|O_CREAT|O_NOATIME|O_DSYNC;

    // Location of the initial log file
    static const char DEFAULT_LOG_FILE[] = "./compressedLog";

    // Determines the size of the output buffer used to store compressed log
    // messages. It should be at least 8MB large to amortize disk seeks and
    // shall not be smaller than STAGING_BUFFER_SIZE.
    static const uint32_t OUTPUT_BUFFER_SIZE = 1<<26;

    // This invariant must be true so that we can output at least one full
    // StagingBuffer per output buffer.
    static_assert(STAGING_BUFFER_SIZE <= OUTPUT_BUFFER_SIZE,
        "OUTPUT_BUFFER_SIZE must be greater than or "
            "equal to the STAGING_BUFFER_SIZE");

    // The threshold at which the consumer should release space back to the
    // producer in the thread-local StagingBuffer. Due to the blocking nature
    // of the producer when it runs out of space, a low value will incur more
    // more blocking but at a shorter duration, whereas a high value will have
    // the opposite effect.
    static const uint32_t RELEASE_THRESHOLD = STAGING_BUFFER_SIZE>>1;

    // How often should the background compression thread wake up to check
    // for more log messages in the StagingBuffers to compress and output.
    // Due to overheads in the kernel, this number will a lower bound and
    // the actual time spent sleeping may be significantly higher.
    static const uint32_t POLL_INTERVAL_NO_WORK_US = 1;

    // How often should the background compression thread wake up and
    // check for more log messages when it's stalled waiting for an IO
    // to complete. Due to overheads in the kernel, this number will
    // be a lower bound and the actual time spent sleeping may be higher.
    static const uint32_t POLL_INTERVAL_DURING_IO_US = 1;

    static const uint32_t BYTES_PER_CACHE_LINE = 64;
}

#endif /* CONFIG_H */

