CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2

# The library is src/; the coordinator build also needs ArduinoJson (third_party/) and
# the mock coordinator (examples/coordinator/).
INCLUDES := -Isrc -Ithird_party -Iexamples/coordinator
LIB_SRCS := src/process.cpp src/engine.cpp
DEPS     := $(wildcard src/*.h examples/coordinator/*.h third_party/*.h) $(LIB_SRCS)

bin/bpe: examples/coordinator $(DEPS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(INCLUDES) $(CXXFLAGS) examples/coordinator/main.cpp $(LIB_SRCS) -o $@

# Unoptimized build with symbols for gdb / :Termdebug.
bin/bpe-debug: examples/coordinator $(DEPS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(INCLUDES) -std=c++17 -Wall -Wextra -g -O0 examples/coordinator/main.cpp $(LIB_SRCS) -o $@

debug: bin/bpe-debug

bin/test_process: tests/test_process.cpp $(DEPS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(INCLUDES) $(CXXFLAGS) tests/test_process.cpp $(LIB_SRCS) -o $@

# Unoptimized test build with symbols for gdb / :Termdebug.
bin/test_process-debug: tests/test_process.cpp $(DEPS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(INCLUDES) -std=c++17 -Wall -Wextra -g -O0 tests/test_process.cpp $(LIB_SRCS) -o $@

debug-test: bin/test_process-debug

bin/test_engine: tests/test_engine.cpp $(DEPS)
	@mkdir -p bin
	$(CXX) $(CPPFLAGS) $(INCLUDES) $(CXXFLAGS) tests/test_engine.cpp $(LIB_SRCS) -o $@

test: bin/test_process bin/test_engine
	./bin/test_process
	./bin/test_engine

run: bin/bpe
	./bin/bpe

# Sources owned by the project; third_party/ is excluded on purpose.
FORMAT_SRCS := $(wildcard src/*.h src/*.cpp tests/*.cpp examples/coordinator/*.h examples/coordinator/*.cpp)
CLANG_FORMAT ?= clang-format

format:
	$(CLANG_FORMAT) -i $(FORMAT_SRCS)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(FORMAT_SRCS)

clean:
	rm -rf bin

.PHONY: run test debug debug-test format format-check clean
