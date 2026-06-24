CXX      = g++
CXXFLAGS = -std=c++20 -Wall -Wextra -Wpedantic -O2

SOURCES = main.cpp Orderbook.cpp

orderbook: $(SOURCES)
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $@

tests: test/test.cpp
	$(CXX) $(CXXFLAGS) -g $< \
		-lgtest -lgtest_main -pthread \
		-o tests

benchmark: bench/bench.cpp
	$(CXX) $(CXXFLAGS) -g $< -pthread -o benchmark

.PHONY: clean
clean:
	rm -f orderbook tests benchmark
