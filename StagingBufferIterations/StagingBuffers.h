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

#ifndef _STAGINGBUFFERS_H_
#define _STAGINGBUFFERS_H_

#include <cstdint>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>

#include "Config.h"

/**
 * This file contains various NanoLog StagingBuffer implementations that have
 * certain  performance optimizations enabled-disabled. Most of them are very
 * similar so only the first class (Basic) will be extensively documented.
 */
namespace StagingBuffers {
    using Lock = std::unique_lock<std::mutex>;

    /**
     * Circular Byte buffer that uses monitor style locking
     */
    struct Basic {
        // Global monitor-style lock
        std::mutex mutex;

        // User-assigned identifier for this buffer
        int id;

        // Offset within the *buffer that the consumer can peek()/pop() from
        int readPos;

        // Offset within the *buffer that the producer can push() to
        int writePos;

        //  Tracks the number of bytes currently in the buffer
        int bytesReadable;

        // Tracks where the offset of where the first invalid byte is
        // in buffer is when a roll-over occurs
        int endOfWrittenSpace;

        // Metrics: Number of bytes push()-ed and pop()-ed
        long bytesPushed;
        long bytesPopped;

        // Contiguous space to store data
        char buffer[NanoLogConfig::STAGING_BUFFER_SIZE];

        Basic(int id)
            : mutex()
            , id(id)
            , readPos(0)
            , writePos(0)
            , bytesReadable(0)
            , endOfWrittenSpace(0)
            , bytesPushed(0)
            , bytesPopped(0)
        {
            bzero(buffer, NanoLogConfig::STAGING_BUFFER_SIZE);
        }

        // true means enqueue was successful
        bool push(const char *data, int nbytes);
        const char* peek(int &bytesAvail);
        void pop(int nbytes);
    };

    template<int bytesPerLog>
    struct StdDeque {
        // Global monitor-style lock
        std::mutex mutex;

        std::condition_variable consumedSome;
        std::condition_variable producedSome;

        struct Element {
            char array[bytesPerLog];
        };

        StdDeque(int id)
            : id(id)
            , deque()
            , consumedSome()
            , producedSome()
        {
            deque.resize(NanoLogConfig::STAGING_BUFFER_SIZE/bytesPerLog);
        }

        bool push(const char *data, int datalen) {
            Lock _(mutex);

            while (deque.size() >= NanoLogConfig::STAGING_BUFFER_SIZE/bytesPerLog) {
                consumedSome.wait(_);
            }

            Element e;
            memcpy(e.array, data, bytesPerLog);
            deque.push_back(e);
            producedSome.notify_one();

            return true;
        }

        void peek(int &bytesAvail) {
            Lock _(mutex);
            bytesAvail = deque.size() * bytesPerLog;
        }

        bool pop(int bytes) {
            Lock _(mutex);
            while (deque.size() <= 0) {
                producedSome.wait(_);
            }

            deque.pop_front();
            consumedSome.notify_all();

            return true;
        }



        int id;
        std::deque<Element> deque;
    };

    struct BasicSpinLock {
        // Atomic flag used to implement a basic spin-lock
        std::atomic_flag lock;

        // See Basic for documentation for the rest of the class
        int id;
        int readPos;
        int writePos;
        int bytesReadable;
        int endOfWrittenSpace;

        long bytesPushed;
        long bytesPopped;

        char buffer[NanoLogConfig::STAGING_BUFFER_SIZE];

        BasicSpinLock(int id)
                : lock(ATOMIC_FLAG_INIT)
                , id(id)
                , readPos(0)
                , writePos(0)
                , bytesReadable(0)
                , endOfWrittenSpace(0)
                , bytesPushed(0)
                , bytesPopped(0)
        {
            lock.clear(std::memory_order_seq_cst);
            bzero(buffer, NanoLogConfig::STAGING_BUFFER_SIZE);
        }

        // true means enqueue was successful
        bool push(const char *data, int nbytes);
        const char* peek(int &bytesAvail);
        void pop(int nbytes);
    };



    struct SignalPoll {
        // monitor-style mutex to lock the entire structure during access
        std::mutex mutex;

        // Hints that some data has been pop()-ed from the buffer
        std::condition_variable consumedSome;

        // Hints that some data has been push()-ed into the buffer
        std::condition_variable producedSome;

        // Check Basic for documentation on the rest of the class
        int id;
        int readPos;
        int writePos;
        int bytesReadable;
        int endOfWrittenSpace;

        long bytesPushed;
        long bytesPopped;

        char buffer[NanoLogConfig::STAGING_BUFFER_SIZE];

        SignalPoll(int id)
                : mutex()
                , producedSome()
                , consumedSome()
                , id(id)
                , readPos(0)
                , writePos(0)
                , bytesReadable(0)
                , endOfWrittenSpace(0)
                , bytesPushed(0)
                , bytesPopped(0)
        {
            bzero(buffer, NanoLogConfig::STAGING_BUFFER_SIZE);
        }

        // true means enqueue was successful
        bool push(const char *data, int nbytes);
        const char* peek(Lock &lock, int &bytesAvail);
        void pop(int nbytes);
    };

}; // StagingBuffers namespace


#endif //_STAGINGBUFFERS_H_
