/* Copyright (c) 2019 Stanford University
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

#include <cassert>
#include <cstring>

#include "StagingBuffers.h"
#include "SeparatedStagingBuffer.h"

namespace StagingBuffers {

/**
 * Copies nbytes of data into the buffer. If successful, it returns true,
 * else false indicates not enough space in the buffer.
 *
 * \param data
 *      Pointer to the data to copy in
 * \param nbytes
 *      Number of bytes to copy from *data
 * \return
 *      true is success; false means insufficient space
 */
bool Basic::push(const char *data, int nbytes)
{
    Lock _(mutex);

    // TRICK: When pushing data, we need to ensure that the positions will
    // NOT overlap after the push as this would indicate 0 readable data.
    // Thus all space checks are performed with <= checks to ensure that
    // the pointers will not overlap after a push.

    // Check for space when reader is in front of the writer (us)
    if (readPos > writePos && readPos - writePos <= nbytes)
        return false;

    // If the reader is behind us, check to see if we need to roll over
    if (readPos <= writePos
                && NanoLogConfig::STAGING_BUFFER_SIZE - writePos < nbytes)
    {
        endOfWrittenSpace = writePos;

        if (readPos == 0)
            return false;

        writePos = 0;
        if (readPos <= nbytes)
            return false;
    }

    std::memcpy(&buffer[writePos], data, nbytes);
    bytesPushed += nbytes;
    bytesReadable += nbytes;
    writePos += nbytes;
    return true;
}

/**
 * Returns the number of contiguous bytes available for consumption from the
 * pointer returned.
 *
 * \param[out] bytesAvail
 *      Number of bytes available  for reading
 * \return
 *      Pointer to read from
 */
const char*
Basic::peek(int &bytesAvail)
{
    Lock _(mutex);

    if (readPos <= writePos) {
        bytesAvail = writePos - readPos;
    } else {
        bytesAvail = endOfWrittenSpace - readPos;

        // roll over
        if (bytesAvail == 0) {
            readPos = 0;
            bytesAvail = writePos - readPos;
        }
    }

    return &buffer[readPos];
}

/**
 * Frees up nbytes for production in the StagingBuffer. This value must NOT be
 * greater than the amount previously peek()-ed.
 *
 * \param nbytes
 *      Number of bytes to free up
 */
void Basic::pop(int nbytes)
{
    Lock _(mutex);
    assert(bytesReadable >= nbytes);

    bytesReadable -= nbytes;
    bytesPopped += nbytes;

    if (readPos < writePos) {
        readPos += nbytes;
        return;
    }

    int firstHalf = endOfWrittenSpace - readPos;
    if (firstHalf >= nbytes) {
        readPos += nbytes;
    } else if (firstHalf == 0) {
        readPos = 0;
    } else {
        nbytes -= firstHalf;
        readPos = nbytes;
    }
}

/**
 * Copies nbytes of data into the buffer. If successful, it returns true,
 * else false indicates not enough space in the buffer.
 *
 * \param data
 *      Pointer to the data to copy in
 * \param nbytes
 *      Number of bytes to copy from *data
 * \return
 *      true is success; false means insufficient space
 */
bool
BasicSpinLock::push(const char *data, int nbytes)
{
    while (lock.test_and_set(std::memory_order_acquire)) {
        PerfUtils::Cycles::rdtsc();
    };  // spin acquire lock with a small (~8ns) backoff

    // TRICK: When pushing data, we need to ensure that the positions will
    // NOT overlap after the push as this would indicate 0 readable data.
    // Thus all space checks are performed with <= checks to ensure that
    // the pointers will not overlap after a push.

    // Check for space when reader is in front of the writer (us)
    if (readPos > writePos && readPos - writePos <= nbytes) {
        lock.clear(std::memory_order_release);
        return false;
    }

    // If the reader is behind us, check to see if we need to roll over
    if (readPos <= writePos
            && NanoLogConfig::STAGING_BUFFER_SIZE - writePos < nbytes)
    {
        endOfWrittenSpace = writePos;

        if (readPos == 0) {
            lock.clear(std::memory_order_release);
            return false;
        }

        writePos = 0;
        if (readPos <= nbytes) {
            lock.clear(std::memory_order_release);
            return false;
        }
    }

    std::memcpy(&buffer[writePos], data, nbytes);
    bytesPushed += nbytes;
    bytesReadable += nbytes;
    writePos += nbytes;

    lock.clear(std::memory_order_release);
    return true;
}

/**
 * Returns the number of contiguous bytes available for consumption from the
 * pointer returned.
 *
 * \param[out] bytesAvail
 *      Number of bytes available  for reading
 * \return
 *      Pointer to read from
 */
const char*
BasicSpinLock::peek(int &bytesAvail)
{
    while (lock.test_and_set(std::memory_order_acquire));  // spin acquire lock

    if (readPos <= writePos) {
        bytesAvail = writePos - readPos;
    } else {
        bytesAvail = endOfWrittenSpace - readPos;

        // roll over
        if (bytesAvail == 0) {
            readPos = 0;
            bytesAvail = writePos - readPos;
        }
    }

    char *ret = &buffer[readPos];
    lock.clear(std::memory_order_release);
    return ret;
}

/**
 * Frees up nbytes for production in the StagingBuffer. This value must NOT be
 * greater than the amount previously peek()-ed.
 *
 * \param nbytes
 *      Number of bytes to free up
 */
void
BasicSpinLock::pop(int nbytes)
{
    while (lock.test_and_set(std::memory_order_acquire));  // spin acquire lock

    bytesReadable -= nbytes;
    bytesPopped += nbytes;

    if (readPos < writePos) {
        readPos += nbytes;
    } else {
        int firstHalf = endOfWrittenSpace - readPos;
        if (firstHalf >= nbytes) {
            readPos += nbytes;
        } else if (firstHalf == 0) {
            readPos = 0;
        } else {
            nbytes -= firstHalf;
            readPos = nbytes;
        }
    }

    lock.clear(std::memory_order_release);
}


/**
 * Copies nbytes of data into the buffer. If there is not enough space, this
 * function will block until enough space is freed for the *data
 *
 * \param data
 *      Pointer to the data to copy in
 * \param nbytes
 *      Number of bytes to copy from *data
 * \return
 *      true is success; false indicates error
 */
bool
SignalPoll::push(const char *data, int nbytes) {
    Lock _(mutex);

    // TRICK: When pushing data, we need to ensure that the positions will
    // NOT overlap after the push as this would indicate 0 readable data.
    // Thus all space checks are performed with <= checks to ensure that
    // the pointers will not overlap after a push.

    while (true) {
        bool hasSpace = true;

        // Check for space when reader is in front of the writer (us)
        if (readPos > writePos && readPos - writePos <= nbytes) {
            hasSpace = false;
        } else if (readPos <= writePos &&
                    NanoLogConfig::STAGING_BUFFER_SIZE - writePos < nbytes)
        {
            // If the reader is behind us, check to see if we need to roll over
            endOfWrittenSpace = writePos;

            if (readPos == 0)
                hasSpace = false;

            writePos = 0;
            if (readPos <= nbytes)
                hasSpace = false;
        }

        if (hasSpace)
            break;

        consumedSome.wait(_);
    }
    producedSome.notify_one();

    std::memcpy(&buffer[writePos], data, nbytes);
    bytesPushed += nbytes;
    bytesReadable += nbytes;
    writePos += nbytes;
    return true;
}

/**
 * Returns the number of contiguous bytes available for consumption from the
 * pointer returned. Note this is an internal function that must be called
 * with a lock
 *
 * \param lock
 *      Monitor lock grabbed for this object
 * \param[out] bytesAvail
 *      Number of bytes available  for reading
 * \return
 *      Pointer to read from
 */
const char*
SignalPoll::peek(Lock &lock, int &bytesAvail) {
    if (readPos <= writePos) {
        bytesAvail = writePos - readPos;
    } else {
        bytesAvail = endOfWrittenSpace - readPos;

        // roll over
        if (bytesAvail == 0) {
            readPos = 0;
            bytesAvail = writePos - readPos;
        }
    }

    return &buffer[readPos];
}

/**
 * Frees up nbytes for production in the StagingBuffer. If there's not enough
 * space to free in the StagingBuffer, this function will block until there is.
 *
 * \param nbytes
 *      Number of bytes to free up
 */
void
SignalPoll::pop(int nbytes) {
    Lock _(mutex);

    while (true) {
        int bytesAvail = 0;
        peek(_, bytesAvail);

        if (bytesAvail >= nbytes)
            break;

        producedSome.wait(_);
    }

    bytesReadable -= nbytes;
    bytesPopped += nbytes;

    if (readPos < writePos) {
        readPos += nbytes;
        consumedSome.notify_all();
        return;
    }

    int firstHalf = endOfWrittenSpace - readPos;
    if (firstHalf >= nbytes) {
        readPos += nbytes;
    } else if (firstHalf == 0) {
        readPos = 0;
    } else {
        nbytes -= firstHalf;
        readPos = nbytes;
    }

    consumedSome.notify_all();
}

}; // StagingBuffers namespace