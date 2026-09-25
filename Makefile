# calc_server: persistent-connection calculator over raw TCP sockets.
#
#   make                   build server, demo client and unit tests
#   make test              unit tests + integration tests
#   make demo              start the server and run the one-connection demo
#   make run               run the server on 127.0.0.1:8080
#   make sanitize          run all tests under AddressSanitizer + UBSan
#   make mutation-check    plant known protocol bugs and confirm the tests catch them
#   make clean

CXX      ?= c++
BUILD    ?= build
CXXFLAGS ?= -O2 -g
PYTHON   ?= python3

# Fixed flags live apart from CXXFLAGS so that overriding CXXFLAGS on the
# command line (e.g. `make sanitize`) cannot drop the language standard.
STD_FLAGS := -std=c++17
WARNINGS  := -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
DEP_FLAGS  = -MMD -MP
INCLUDES  := -Iinclude

LIB_SRCS  := $(filter-out src/main.cpp,$(wildcard src/*.cpp))
LIB_OBJS  := $(LIB_SRCS:%.cpp=$(BUILD)/obj/%.o)
MAIN_OBJ  := $(BUILD)/obj/src/main.o
DEMO_OBJ  := $(BUILD)/obj/tools/demo_client.o
TEST_OBJS := $(patsubst %.cpp,$(BUILD)/obj/%.o,$(wildcard tests/*.cpp))
ALL_OBJS  := $(LIB_OBJS) $(MAIN_OBJ) $(DEMO_OBJ) $(TEST_OBJS)

SERVER := $(BUILD)/calc_server
DEMO   := $(BUILD)/demo_client
UNIT   := $(BUILD)/unit_tests

.PHONY: all test unit-test integration-test demo run sanitize mutation-check clean

all: $(SERVER) $(DEMO) $(UNIT)

$(SERVER): $(LIB_OBJS) $(MAIN_OBJ)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

$(DEMO): $(DEMO_OBJ)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

$(UNIT): $(LIB_OBJS) $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

$(BUILD)/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(STD_FLAGS) $(WARNINGS) $(INCLUDES) $(CXXFLAGS) $(DEP_FLAGS) -c $< -o $@

test: unit-test integration-test

unit-test: $(UNIT)
	./$(UNIT)

integration-test: $(SERVER)
	CALC_SERVER=$(SERVER) $(PYTHON) tests/integration_test.py

demo: $(SERVER) $(DEMO)
	BUILD=$(BUILD) ./scripts/demo.sh

run: $(SERVER)
	./$(SERVER)

# Any sanitizer finding aborts the run (-fno-sanitize-recover). If the
# AddressSanitizer runtime is broken on your toolchain (it hangs at startup
# with some Apple clang / macOS combinations), use SANITIZERS=undefined.
SANITIZERS ?= address,undefined
sanitize:
	$(MAKE) BUILD=build-sanitize \
		CXXFLAGS="-O1 -g -fsanitize=$(SANITIZERS) -fno-sanitize-recover=all -fno-omit-frame-pointer" \
		LDFLAGS="-fsanitize=$(SANITIZERS)" test

mutation-check:
	$(PYTHON) scripts/mutation_check.py

clean:
	rm -rf build build-sanitize

-include $(ALL_OBJS:.o=.d)
