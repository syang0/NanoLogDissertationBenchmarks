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

#include <cstring>
#include <unistd.h>
#include <thread>
#include <vector>

#include <xmmintrin.h>
#include <pthread.h>

#include "PerfUtils/TimeTrace.h"
#include "PerfUtils/Util.h"


#include "StagingBuffers.h"
#include "SeparatedStagingBuffer.h"

using namespace NanoLogConfig;

// This function takes somewhere between 9-10 on average.
static uint64_t cntr = 0;
void function(uint64_t cycles) {
    cntr = cycles*2%100 + PerfUtils::Cycles::rdtsc();
}

struct Metrics {
    int threadId;
    uint64_t numOps;
    uint64_t totalCycles;

    Metrics()
        : threadId(0)
        , numOps(0)
        , totalCycles(0)
    { }

    double getAvgLatencyInNs() {
        return (PerfUtils::Cycles::toSeconds(totalCycles)*1.0e9)/numOps;
    }
};

template<typename Buffer>
void doPushes(int iterations, Buffer *sb)
{
    for (int i = 0; i < iterations; ++i) {
        if (!sb->push(datum, datum_len))
            --i;
    }
}



template<typename Buffer>
void doConsumes(int iterations, Buffer **sbs, int numBuffers)
{
    int numConsumed = 0;
    while (numConsumed < iterations) {
        for (int j = 0; j < numBuffers; j++) {
            int bytesAvail;
            sbs[j]->peek(bytesAvail);

            if (bytesAvail >= datum_len) {
                sbs[j]->pop(datum_len);
                PerfUtils::Cycles::rdtsc();
                ++numConsumed;
            }
        }
    }
}

template<typename Buffer>
void doPushesCond(int iterations, Buffer *sb)
{
    for (int i = 0; i < iterations; ++i) {
        sb->push(datum, datum_len);
    }
}

template<typename Buffer>
void doConsumesCond(int iterations, Buffer **sbs, int numBuffers)
{
    int numConsumed = 0;
    int numConsumedPerBuffer[numBuffers] = {0};
    int numToBeConsumedPerBuffer = iterations/numBuffers;

    while (numConsumed < iterations) {
        for (int j = 0; j < numBuffers; j++) {
            if (numConsumedPerBuffer[j] >= numToBeConsumedPerBuffer)
                continue;

            sbs[j]->pop(datum_len);
            PerfUtils::Cycles::rdtsc();
            ++numConsumed;
        }
    }
}

template<typename Buffer>
void doPushesTwoStage(int iterations, Buffer *sb)
{
    for (int i = 0; i < iterations; ++i) {
        char *pos = sb->reserveProducerSpace(datum_len);
        std::memcpy(pos, datum, datum_len);
        sb->finishReservation(datum_len);

    }
}

template<typename Buffer>
void doConsumesTwoStage(int iterations, Buffer **sbs, int numBuffers)
{
    int numConsumed = 0;
    while (numConsumed < iterations) {
        for (int j = 0; j < numBuffers; j++) {
            uint64_t bytesAvail;
            sbs[j]->peek(&bytesAvail);

            if (bytesAvail >= datum_len) {
                sbs[j]->consume(datum_len);
                PerfUtils::Cycles::rdtsc();
                ++numConsumed;
            }
        }
    }
}

template<typename Buffer>
void doConsumesTwoStageBatched(int iterations, Buffer **sbs, int numBuffers)
{
    int numConsumed = 0;
    while (numConsumed < iterations) {
        for (int j = 0; j < numBuffers; j++) {
            uint64_t bytesAvail;
            sbs[j]->peek(&bytesAvail);

            if (bytesAvail >= datum_len) {
                uint64_t itemsConsumed = bytesAvail/datum_len;
                for (uint64_t i = 0; i < itemsConsumed; ++i)
                    PerfUtils::Cycles::rdtsc();

                sbs[j]->consume(bytesAvail);
                numConsumed += itemsConsumed;
            }
        }
    }
}

template<typename Buffer>
void pusherMain(int id, pthread_barrier_t *barrier, Buffer *sb,
                void (*doPushes)(int, Buffer *), Metrics *m)
{
    uint64_t start, stop;
    double time;

    PerfUtils::Util::pinThreadToCore(id);
    pthread_barrier_wait(barrier);

    start = PerfUtils::Cycles::rdtsc();
    doPushes(ITERATIONS/BENCHMARK_THREADS, sb);
    stop = PerfUtils::Cycles::rdtsc();

    m->threadId = id;
    m->numOps = ITERATIONS/BENCHMARK_THREADS;
    m->totalCycles = stop - start;
}

template<typename Buffer>
void runTest(const char *testName,
                bool runIndividualBuffers,
                void (*benchOp)(int,Buffer*),
                void (*consumeOp)(int,Buffer**,int)) {
    Metrics popMetrics = {};

    pthread_barrier_t barrier;
    if (pthread_barrier_init(&barrier, NULL, BENCHMARK_THREADS + 1)) {
        printf("pthread error\r\n");
    }

    std::vector<std::thread> threads;
    Buffer *buffers[BENCHMARK_THREADS];
    Metrics pushMetrics[BENCHMARK_THREADS];
    for (int i = 0; i < BENCHMARK_THREADS; ++i) {
        buffers[i] = new Buffer(i);
        pushMetrics[i] = {};

        Buffer *bufferToUse = (runIndividualBuffers) ? buffers[i] : buffers[0];
        threads.emplace_back(pusherMain<Buffer>, i,
                             &barrier, bufferToUse, benchOp, &pushMetrics[i]);
    }

    // Consumer Start
    {
        uint64_t consumations = BENCHMARK_THREADS*(ITERATIONS/BENCHMARK_THREADS);
        PerfUtils::Util::pinThreadToCore(BENCHMARK_THREADS);
        pthread_barrier_wait(&barrier);

        uint64_t start = PerfUtils::Cycles::rdtsc();
        consumeOp(consumations, buffers,
                    (runIndividualBuffers) ? BENCHMARK_THREADS : 1);
        uint64_t stop = PerfUtils::Cycles::rdtsc();

        popMetrics.numOps = consumations;
        popMetrics.totalCycles = stop - start;
    }

    // End of test teardown
    for (int i = 0; i < threads.size(); ++i)
        if (threads[i].joinable())
            threads.at(i).join();

    for (int i = 0; i < BENCHMARK_THREADS; ++i) {
        free(buffers[i]);
        buffers[i] = nullptr;
    }

    // Combine and print metrics
    Metrics pushTotals = {};
    for (int i = 0 ; i < BENCHMARK_THREADS; ++i) {
        pushTotals.totalCycles += pushMetrics[i].totalCycles;
        pushTotals.numOps += pushMetrics[i].numOps;
    }

    printf("%-19s %10s %10lu %15.2lf %15.2lf\r\n",
           testName,
           runIndividualBuffers ? "false" : "true",
           popMetrics.numOps,
           popMetrics.getAvgLatencyInNs(),
           pushTotals.getAvgLatencyInNs()/BENCHMARK_THREADS);
}

int main(int argc, char** argv) {
    constexpr uint64_t numOps = BENCHMARK_THREADS*(ITERATIONS/BENCHMARK_THREADS);
    char hostname[256];
    if (gethostname(hostname, 256))
        bzero(hostname, 256); // Null out the string if an error occurs

    printf("# Benchmarks the NanoLog StagingBuffer with certain optimizations "
           "disabled.\r\n"
           "# It mocks the NanoLog operations by utilizing multiple threads to"
           " push fixed\r\n"
           "# size data to a buffer and a separate thread to pop them back out."
           "\r\n"
           "# The average operation time is reported.\r\n"
           "#\r\n"
           "# - Configuration -\r\n"
           "# Number of push operations: %0.2lf KOps\r\n"
           "# Number of threads: %d\r\n"
           "# Datum: \"%s\"\r\n"
           "# Datum size: %ld Bytes\r\n"
           "# Staging Buffer Size: %0.3lf KB\r\n"
           "# Benchmark machine hostname: %s",
           numOps/1.0e3,
           BENCHMARK_THREADS,
           datum,
           datum_len,
           NanoLogConfig::STAGING_BUFFER_SIZE/1.0e3,
           hostname);

    printf("\r\n\r\n# %-18s %10s %10s %15s %15s\r\n",
            "Condition", "Global", "Num Ops", "Consume (ns)", "Push Avg (ns)");

    runTest<StagingBuffers::Basic>("Basic", true, &doPushes, &doConsumes);
    runTest<StagingBuffers::Basic>("Basic", false, &doPushes, &doConsumes);
    runTest<StagingBuffers::StdDeque<datum_len>>("Deque", true, &doPushes, &doConsumes);
    runTest<StagingBuffers::StdDeque<datum_len>>("Deque", false, &doPushes, &doConsumes);
    runTest<StagingBuffers::SignalPoll>("Signaler", true, &doPushesCond, &doConsumesCond);
    runTest<StagingBuffers::SignalPoll>("Signaler", false, &doPushesCond, &doConsumesCond);
    runTest<StagingBuffers::BasicSpinLock>("BasicSpinLock", true, &doPushes, &doConsumes);
    runTest<StagingBuffers::BasicSpinLock>("BasicSpinLock", false, &doPushes, &doConsumes);
    runTest<Alternatives::StagingBuffer<0>>("Full No Batch/FS", true, &doPushesTwoStage, &doConsumesTwoStage);
    runTest<Alternatives::StagingBuffer<0>>("Full False Sharing", true, &doPushesTwoStage, &doConsumesTwoStageBatched);
    runTest<Alternatives::StagingBuffer<64>>("Full No Batched", true, &doPushesTwoStage, &doConsumesTwoStage);
    runTest<Alternatives::StagingBuffer<64>>("Full", true, &doPushesTwoStage, &doConsumesTwoStageBatched);
}