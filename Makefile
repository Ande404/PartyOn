# PartyOn build
#   make          build server and client into bin/
#   make server   build bin/partyon-server
#   make client   build bin/partyon-client
#   make test     build and run the unit tests
#   make tsan     run the unit tests under ThreadSanitizer (data race detector)
#   make clean    remove build output

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
CPPFLAGS += -Iserver
LDLIBS   += -pthread

ifeq ($(OS),Windows_NT)
    LDLIBS += -lws2_32
    EXE := .exe
endif

BIN_DIR   := bin
BUILD_DIR := build

# Everything in the server except main.cpp, so the unit tests can link against it
SERVER_LIB_SRCS := server/LineFramer.cpp server/Protocol.cpp server/AppState.cpp \
                   server/Dispatcher.cpp server/Logger.cpp server/Session.cpp
SERVER_LIB_OBJS := $(SERVER_LIB_SRCS:%.cpp=$(BUILD_DIR)/%.o)
SERVER_MAIN_OBJ := $(BUILD_DIR)/server/main.o
TEST_NAMES      := test_framer test_state test_concurrency
TEST_OBJS       := $(TEST_NAMES:%=$(BUILD_DIR)/tests/%.o)

SERVER := $(BIN_DIR)/partyon-server$(EXE)
CLIENT := $(BIN_DIR)/partyon-client$(EXE)
TESTS  := $(TEST_NAMES:%=$(BIN_DIR)/%$(EXE))

.PHONY: all server client test tsan clean
.SECONDARY:

all: server client

server: $(SERVER)

client: $(CLIENT)

$(SERVER): $(SERVER_MAIN_OBJ) $(SERVER_LIB_OBJS) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(CLIENT): client/client.cpp | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

$(BIN_DIR)/test_%$(EXE): $(BUILD_DIR)/tests/test_%.o $(SERVER_LIB_OBJS) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

test: $(TESTS)
	@for t in $(TESTS); do echo "== $$t"; ./$$t || exit 1; done

# Separate build and bin directories so sanitizer objects never mix with normal ones
tsan:
	$(MAKE) BUILD_DIR=build-tsan BIN_DIR=bin-tsan \
		CXXFLAGS="-std=c++17 -Wall -Wextra -O1 -g -fsanitize=thread" \
		LDLIBS="-pthread -fsanitize=thread" test

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BIN_DIR):
	mkdir -p $@

clean:
	rm -rf $(BIN_DIR) $(BUILD_DIR) bin-tsan build-tsan

-include $(SERVER_LIB_OBJS:.o=.d) $(SERVER_MAIN_OBJ:.o=.d) $(TEST_OBJS:.o=.d)
