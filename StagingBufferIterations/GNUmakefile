SRCS=main.cc StagingBuffers.cc
OBJECTS:=$(SRCS:.cc=.o)

TEST_SRC=StagingBufferTest.cc StagingBuffers.cc
TEST_OBJS:=$(TEST_SRC:.cc=.o)

all: benchmark

include $(SRCS:.cc=.d)
include $(TEST_SRC:.cc=.d)

GTEST_DIR=../googletest/googletest
INCLUDES= -I../PerfUtils/include -I${GTEST_DIR}/include
LDFLAGS= -L../PerfUtils/lib -L. -lPerfUtils -lpthread -lgtest
CXXFLAGS= -std=c++17 -DNDEBUG -O3 -g

benchmark: $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ $(LDFLAGS) -o benchmark

test: $(TEST_OBJS)
	$(CXX) -std=c++17 -g $(INCLUDES) $^ $(GTEST_DIR)/src/gtest_main.cc $(LDFLAGS) -o test

libgtest.a:
	$(CXX) -std=c++11 -isystem $(GTEST_DIR)/include -I$(GTEST_DIR) -c $(GTEST_DIR)/src/gtest-all.cc $(LIBS)
	ar -rv libgtest.a gtest-all.o
	@rm gtest-all.o

%.o: %.cc %.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

%.o: %.cc
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

%.d: %.cc
	@set -e; rm -f $@; \
	$(CXX) -MM $(CXXFLAGS) $(INCLUDES) $< > $@.$$$$; \
	sed 's,\($*\)\.o[ :]*,\1.o $@ : ,g' < $@.$$$$ > $@; \
	rm -f $@.$$$$

clean:
	rm -f $(OBJECTS) *.d test benchmark