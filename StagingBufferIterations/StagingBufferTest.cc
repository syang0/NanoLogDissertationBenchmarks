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

#include "gtest/gtest.h"

#include "StagingBuffers.h"

namespace {

// The fixture for testing class Foo.
class StagingBufferTest : public ::testing::Test {
protected:
    // You can remove any or all of the following functions if its body
    // is empty.

    StagingBufferTest()
            : testFile("packerTestFile.bin"), buffer_space(),
              buffer(buffer_space) {
        // You can do set-up work for each test here.
    }

    virtual ~StagingBufferTest() {
        // You can do clean-up work that doesn't throw exceptions here.
    }

    // If the constructor and destructor are not enough for setting up
    // and cleaning up each test, you can define the following methods:

    virtual void SetUp() {
        // Code here will be called immediately after the constructor (right
        // before each test).
    }

    virtual void TearDown() {
        // Code here will be called immediately after each test (right
        // before the destructor).
        std::remove(testFile);
    }

    // Objects declared here can be used by all tests in the test case for Foo.

    const char *testFile;
    char buffer_space[10000];
    char *buffer = buffer_space;
};

TEST_F(StagingBufferTest, BasicBasic) {
    StagingBuffers::Basic basic(0);
    char buffer[100];

    bzero(basic.buffer, StagingBuffers::STAGING_BUFFER_BYTES);
    bzero(buffer, 100);

    int bytesAvail;
    EXPECT_EQ(basic.buffer, basic.peek(bytesAvail));
    EXPECT_EQ(0, bytesAvail);

    std::memcpy(buffer, "abcdeabcdeabcd", 15);
    EXPECT_TRUE(basic.push(buffer, 15));

    // Peek twice and expect the same thing twice
    EXPECT_EQ(basic.buffer, basic.peek(bytesAvail));
    EXPECT_EQ(15, bytesAvail);

    EXPECT_EQ(basic.buffer, basic.peek(bytesAvail));
    EXPECT_EQ(15, bytesAvail);

    // Push some more data
    std::memcpy(buffer, "123456789", 10);
    EXPECT_TRUE(basic.push(buffer, 10));

    EXPECT_EQ(basic.buffer, basic.peek(bytesAvail));
    EXPECT_EQ(25, bytesAvail);

    // Check the data
    const char *eatMe = basic.peek(bytesAvail);
    EXPECT_STREQ("abcdeabcdeabcd", eatMe);
    EXPECT_STREQ("123456789", eatMe + 15);

    // Check for internal consistency
    EXPECT_EQ(0, basic.readPos);
    EXPECT_EQ(25, basic.writePos);
    EXPECT_EQ(25, basic.bytesReadable);
    EXPECT_EQ(0, basic.endOfWrittenSpace);

    // Now let's try consuming the data *gulp*
    eatMe = basic.peek(bytesAvail);
    EXPECT_EQ(25, bytesAvail);
    EXPECT_EQ(basic.buffer, eatMe);
    basic.pop(15);

    // External + internal consistency
    eatMe = basic.peek(bytesAvail);
    EXPECT_EQ(10, bytesAvail);
    EXPECT_EQ(basic.buffer + 15, eatMe);
    EXPECT_EQ(15, basic.readPos);
    EXPECT_EQ(25, basic.writePos);
    EXPECT_EQ(10, basic.bytesReadable);
    EXPECT_EQ(0, basic.endOfWrittenSpace);

    // Consume the rest
    basic.pop(10);

    eatMe = basic.peek(bytesAvail);
    EXPECT_EQ(0, bytesAvail);
    EXPECT_EQ(basic.buffer + 25, eatMe);
    EXPECT_EQ(25, basic.readPos);
    EXPECT_EQ(25, basic.writePos);
    EXPECT_EQ(0, basic.bytesReadable);
    EXPECT_EQ(0, basic.endOfWrittenSpace);

    // When we try to enqueue something large and the buffer is empty, try roll
    ASSERT_FALSE(basic.push(basic.buffer,
                            StagingBuffers::STAGING_BUFFER_BYTES  + 1));
    EXPECT_EQ(25, basic.readPos);
    EXPECT_EQ(0, basic.writePos);
    EXPECT_EQ(0, basic.bytesReadable);
    EXPECT_EQ(25, basic.endOfWrittenSpace);

    EXPECT_EQ(basic.buffer, basic.peek(bytesAvail));
    EXPECT_EQ(0, bytesAvail);

    // Now let's fill the buffers
    EXPECT_TRUE(basic.push(basic.buffer, StagingBuffers::STAGING_BUFFER_BYTES));
    EXPECT_FALSE(basic.push(buffer, 1));

    EXPECT_EQ(basic.buffer, basic.peek(bytesAvail));
    EXPECT_EQ(StagingBuffers::STAGING_BUFFER_BYTES, bytesAvail);

    // Eat a little and try to push more.
    basic.pop(50);
    EXPECT_EQ(basic.buffer + 50, basic.peek(bytesAvail));
    EXPECT_EQ(StagingBuffers::STAGING_BUFFER_BYTES - 50, bytesAvail);

    EXPECT_FALSE(basic.push(buffer, 51));
    EXPECT_EQ(50, basic.readPos);
    EXPECT_EQ(0, basic.writePos);
    EXPECT_EQ(bytesAvail, basic.bytesReadable);
    EXPECT_EQ(StagingBuffers::STAGING_BUFFER_BYTES, basic.endOfWrittenSpace);

    EXPECT_TRUE(basic.push(buffer, 20));
    EXPECT_FALSE(basic.push(buffer, 31));
    // avail didn't increase since we can only read contig data
    EXPECT_EQ(basic.buffer + 50, basic.peek(bytesAvail));
    basic.pop(bytesAvail);
    EXPECT_EQ(basic.buffer, basic.peek(bytesAvail));
    EXPECT_EQ(20, bytesAvail);

    // Last test, try to have a straddled roll-over
    basic.readPos = 100;
    basic.writePos = StagingBuffers::STAGING_BUFFER_BYTES - 50;
    basic.bytesReadable = StagingBuffers::STAGING_BUFFER_BYTES - 150;
    basic.endOfWrittenSpace = 0;

    ASSERT_TRUE(basic.push(buffer, 75));

    EXPECT_EQ(100, basic.readPos);
    EXPECT_EQ(75, basic.writePos);
    EXPECT_EQ(StagingBuffers::STAGING_BUFFER_BYTES - 75,
                basic.bytesReadable);
    EXPECT_EQ(StagingBuffers::STAGING_BUFFER_BYTES - 50,
                basic.endOfWrittenSpace);
}

TEST_F(StagingBufferTest, BasicHalfTester) {
    StagingBuffers::Basic basic(0);

    basic.endOfWrittenSpace = 10;
    basic.bytesReadable = 10-8+5;
    basic.readPos = 8;
    basic.writePos = 5;

    basic.pop(3);

    EXPECT_EQ(1, basic.readPos);
    EXPECT_EQ(5, basic.writePos);
    EXPECT_EQ(10-8+5-3, basic.bytesReadable);
    EXPECT_EQ(10, basic.endOfWrittenSpace);
    EXPECT_EQ(3, basic.bytesPopped);
}

} // empty namespace