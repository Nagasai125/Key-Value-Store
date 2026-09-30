CXX ?= c++
CPPFLAGS += -Iinclude
CXXFLAGS ?= -O3 -DNDEBUG
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -pthread
LDFLAGS += -pthread
BUILD ?= build

.PHONY: all test bench sanitize clean
all: $(BUILD)/kv_cli $(BUILD)/kv_tests $(BUILD)/kv_bench

$(BUILD):
	mkdir -p $@

$(BUILD)/store.o: src/store.cpp include/kv/store.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/kv_cli: src/cli.cpp $(BUILD)/store.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

$(BUILD)/kv_tests: tests/store_test.cpp $(BUILD)/store.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

$(BUILD)/kv_bench: bench/benchmark.cpp $(BUILD)/store.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $^ $(LDFLAGS) -o $@

test: $(BUILD)/kv_tests
	./$(BUILD)/kv_tests

bench: $(BUILD)/kv_bench
	./$(BUILD)/kv_bench

sanitize:
	$(MAKE) BUILD=build-sanitize CXXFLAGS='-O1 -g -std=c++17 -Wall -Wextra -Wpedantic -pthread -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-pthread -fsanitize=address,undefined' test

clean:
	rm -rf build build-sanitize
