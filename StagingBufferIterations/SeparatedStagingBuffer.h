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

#ifndef RUNTIME_SEPARATEDSTAGINGBUFFER_H
#define RUNTIME_SEPARATEDSTAGINGBUFFER_H

#include <cstdint>

#include "Config.h"
#include "PerfUtils/Cycles.h"
#include "Fence.h"

namespace Alternatives {
// Returns the number of elements in a statically allocated array.
template<class T, size_t N>
constexpr size_t arraySize(T (&)[N]) { return N; }

/**
 * Implements a circular FIFO producer/consumer byte queue that is used
 * to hold the dynamic information of a NanoLog log statement (producer)
 * as it waits for compression via the NanoLog background thread
 * (consumer). There exists a StagingBuffer for every thread that uses
 * the NanoLog system.
 *
 * The implementation is the live version of StagingBuffer that exists
 * in the full NanoLog system.
 */
template<int CacheLineSpacerBytes>
class StagingBuffer {
public:
    /**
     * Peek at the data available for consumption within the stagingBuffer.
     * The consumer should also invoke consume() to release space back
     * to the producer. This can and should be done piece-wise where a
     * large peek can be consume()-ed in smaller pieces to prevent blocking
     * the producer.
     *
     * \param[out] bytesAvailable
     *      Number of bytes consumable
     * \return
     *      Pointer to the consumable space
     */
    char *
    peek(uint64_t *bytesAvailable) {
        // Save a consistent copy of recordHead
        char *cachedRecordHead = producerPos;

        if (cachedRecordHead < consumerPos) {
            // Prevent reading new producerPos but old endOf...
            NanoLogInternal::Fence::lfence();
            *bytesAvailable = endOfRecordedSpace - consumerPos;

            if (*bytesAvailable > 0)
                return consumerPos;

            // Roll over
            consumerPos = storage;
        }

        *bytesAvailable = cachedRecordHead - consumerPos;
        return consumerPos;
    }

    /**
     * Consumes the next nbytes in the StagingBuffer and frees it back
     * for the producer to reuse. nbytes must be less than what is
     * returned by peek().
     *
     * \param nbytes
     *      Number of bytes to return back to the producer
     */
    inline void
    consume(uint64_t nbytes) {
        // Make sure consumer reads finish before bump
        NanoLogInternal::Fence::lfence();
        consumerPos += nbytes;
//        consumerPos.fetch_add(nbytes, std::memory_order_release);
    }

    /**
     * Returns true if it's safe for the compression thread to delete
     * the StagingBuffer and remove it from the global vector.
     *
     * \return
     *      true if its safe to delete the StagingBuffer
     */
    bool
    checkCanDelete() {
        return shouldDeallocate && consumerPos == producerPos;
    }


    uint32_t getId() {
        return id;
    }

    StagingBuffer(uint32_t bufferId)
        : producerPos(nullptr)
        , endOfRecordedSpace(nullptr)
        , minFreeSpace(NanoLogConfig::STAGING_BUFFER_SIZE)
        , cyclesProducerBlocked(0)
        , numTimesProducerBlocked(0)
        , numAllocations(0)
#ifdef RECORD_PRODUCER_STATS
        , cyclesProducerBlockedDist()
#endif
        , cyclesIn10Ns(PerfUtils::Cycles::fromNanoseconds(10))
        , cacheLineSpacer()
        , consumerPos(nullptr)
        , shouldDeallocate(false)
        , id(bufferId)
        , storage(nullptr)
    {
        storage = static_cast<char*>(malloc(NanoLogConfig::STAGING_BUFFER_SIZE));
        assert(storage);

        producerPos = consumerPos = storage;
        endOfRecordedSpace = storage + NanoLogConfig::STAGING_BUFFER_SIZE;

#ifdef RECORD_PRODUCER_STATS
        for (size_t i = 0; i < arraySize(cyclesProducerBlockedDist); ++i)
        {
            cyclesProducerBlockedDist[i] = 0;
        }
#endif
    }

    ~StagingBuffer() {
        if (storage != nullptr) {
            free(storage);
            storage = nullptr;
        }
    }

    public:

    /**
     * Attempt to reserve contiguous space for the producer without making it
     * visible to the consumer (See reserveProducerSpace).
     *
     * This is the slow path of reserveProducerSpace that checks for free space
     * within storage[] that involves touching variable shared with the compression
     * thread and thus causing potential cache-coherency delays.
     *
     * \param nbytes
     *      Number of contiguous bytes to reserve.
     *
     * \param blocking
     *      Test parameter that indicates that the function should
     *      return with a nullptr rather than block when there's
     *      not enough space.
     *
     * \return
     *      A pointer into storage[] that can be written to by the producer for
     *      at least nbytes.
     */
    char*
    reserveSpaceInternal(size_t nbytes, bool blocking= false)
    {
        const char *endOfBuffer = storage + NanoLogConfig::STAGING_BUFFER_SIZE;

#ifdef RECORD_PRODUCER_STATS
        uint64_t start = PerfUtils::Cycles::rdtsc();
#endif

        // There's a subtle point here, all the checks for remaining
        // space are strictly < or >, not <= or => because if we allow
        // the record and print positions to overlap, we can't tell
        // if the buffer either completely full or completely empty.
        // Doing this check here ensures that == means completely empty.
        while (minFreeSpace <= nbytes) {
            // Since readHead can be updated in a different thread, we
            // save a consistent copy of it here to do calculations on
            char *cachedReadPos = consumerPos;

            if (cachedReadPos <= producerPos) {
                minFreeSpace = endOfBuffer - producerPos;

                if (minFreeSpace > nbytes)
                    break;

                // Not enough space at the end of the buffer; wrap around
                endOfRecordedSpace = producerPos;

                // Prevent the roll over if it overlaps the two positions because
                // that would imply the buffer is completely empty when it's not.
                if (cachedReadPos != storage) {
                    // prevents producerPos from updating before endOfRecordedSpace
                    NanoLogInternal::Fence::sfence();
                    producerPos = storage;
                    minFreeSpace = cachedReadPos - producerPos;
                }
            } else {
                minFreeSpace = cachedReadPos - producerPos;
            }

            // Needed to prevent infinite loops in tests
            if (!blocking && minFreeSpace <= nbytes)
                return nullptr;
        }

#ifdef RECORD_PRODUCER_STATS
        uint64_t cyclesBlocked = PerfUtils::Cycles::rdtsc() - start;
    cyclesProducerBlocked += cyclesBlocked;

    size_t maxIndex = Util::arraySize(cyclesProducerBlockedDist) - 1;
    size_t index = std::min(cyclesBlocked/cyclesIn10Ns, maxIndex);
    ++(cyclesProducerBlockedDist[index]);
#endif

        ++numTimesProducerBlocked;
        return producerPos;
    }

    /**
     * Attempt to reserve contiguous space for the producer without
     * making it visible to the consumer. The caller should invoke
     * finishReservation() before invoking reserveProducerSpace()
     * again to make the bytes reserved visible to the consumer.
     *
     * This mechanism is in place to allow the producer to initialize
     * the contents of the reservation before exposing it to the
     * consumer. This function will block behind the consumer if
     * there's not enough space.
     *
     * \param nbytes
     *      Number of bytes to allocate
     *
     * \return
     *      Pointer to at least nbytes of contiguous space
     */
    inline char *
    reserveProducerSpace(size_t nbytes) {
        ++numAllocations;

        // Fast in-line path
        if (nbytes < minFreeSpace)
            return producerPos;

        // Slow allocation
        return reserveSpaceInternal(nbytes, true);
    }

    /**
     * Complement to reserveProducerSpace that makes nbytes starting
     * from the return of reserveProducerSpace visible to the consumer.
     *
     * \param nbytes
     *      Number of bytes to expose to the consumer
     */
    inline void
    finishReservation(size_t nbytes) {
        assert(nbytes < minFreeSpace);
        assert(producerPos + nbytes <
               storage + NanoLogConfig::STAGING_BUFFER_SIZE);

        // Ensures producer finishes writes before bump
        NanoLogInternal:: Fence::sfence();
        minFreeSpace -= nbytes;
        producerPos += nbytes;
    }

    // Position within storage[] where the producer may place new data
    char *producerPos;

    // Marks the end of valid data for the consumer. Set by the producer
    // on a roll-over
    char *endOfRecordedSpace;

    // Lower bound on the number of bytes the producer can allocate w/o
    // rolling over the producerPos or stalling behind the consumer
    uint64_t minFreeSpace;

    // Number of cycles producer was blocked while waiting for space to
    // free up in the StagingBuffer for an allocation.
    uint64_t cyclesProducerBlocked;

    // Number of times the producer was blocked while waiting for space
    // to free up in the StagingBuffer for an allocation
    uint32_t numTimesProducerBlocked;

    // Number of alloc()'s performed
    uint64_t numAllocations;

#ifdef RECORD_PRODUCER_STATS
    // Distribution of the number of times Producer was blocked
    // allocating space in 10ns increments. The last slot includes
    // all times greater than the last increment.
    uint32_t cyclesProducerBlockedDist[20];
#endif

    // Number of Cycles in 10ns. This is used to avoid the expensive
    // Cycles::toNanoseconds() call to calculate the bucket in the
    // cyclesProducerBlockedDist distribution.
    uint64_t cyclesIn10Ns;

    // An extra cache-line to separate the variables that are primarily
    // updated/read by the producer (above) from the ones by the
    // consumer(below)
    char cacheLineSpacer[CacheLineSpacerBytes];

    // Position within the storage buffer where the consumer will consume
    // the next bytes from. This value is only updated by the consumer.
    char *volatile consumerPos;

    // Indicates that the thread owning this StagingBuffer has been
    // destructed (i.e. no more messages will be logged to it) and thus
    // should be cleaned up once the buffer has been emptied by the
    // compression thread.
    bool shouldDeallocate;

    // Uniquely identifies this StagingBuffer for this execution. It's
    // similar to ThreadId, but is only assigned to threads that NANO_LOG).
    uint32_t id;

    // Backing store used to implement the circular queue
    char *storage;
//    char storage[NanoLogConfig::STAGING_BUFFER_SIZE];
};

}; // Namespace alternatives



#endif //RUNTIME_SEPARATEDSTAGINGBUFFER_H
