CXX = g++

COMMON_FLAGS = -std=c++20 -Wall -Wextra -Wpedantic
DEBUG_FLAGS = -g -O0
RELEASE_FLAGS = -O2

SOURCES = main.cpp Orderbook.cpp


orderbook: $(SOURCES)
	$(CXX) $(COMMON_FLAGS) $(RELEASE_FLAGS) $(SOURCES) -o $@


debug: $(SOURCES)
	$(CXX) $(COMMON_FLAGS) $(DEBUG_FLAGS) \
	-fsanitize=address,undefined \
	-fno-omit-frame-pointer \
	$(SOURCES) -o debug

# to work in WSL2, instead run: setarch x86_64 -R ./tsan
tsan: test/tsan_harness.cpp Orderbook.cpp
	$(CXX) $(COMMON_FLAGS) $(DEBUG_FLAGS) \
	-fsanitize=thread \
	-fno-omit-frame-pointer \
	test/tsan_harness.cpp Orderbook.cpp -o tsan


tests: test/test.cpp
	$(CXX) $(COMMON_FLAGS) $(DEBUG_FLAGS) \
	test/test.cpp \
	-lgtest -lgtest_main -pthread \
	-o tests


benchmark: bench/bench.cpp
	$(CXX) $(COMMON_FLAGS) $(RELEASE_FLAGS) \
	bench/bench.cpp \
	-pthread \
	-o benchmark


.PHONY: clean
clean:
	rm -f orderbook debug tsan tests benchmark
