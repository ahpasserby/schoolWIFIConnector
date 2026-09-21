# schoolwifi - campus captive-portal connector for macOS
#
# No CMake, no package manager: everything it links against ships with macOS
# and the Command Line Tools.
#   make            build build/schoolwifi
#   make install    copy it to $(PREFIX)/bin (default /usr/local/bin)
#   make test       build and run the unit tests
#   make clean      remove build artifacts

CXX      ?= clang++
PREFIX   ?= /usr/local
BUILD    ?= build

CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
CXXFLAGS += -Iinclude
LDFLAGS  += -lcurl \
            -lresolv \
            -framework CoreWLAN \
            -framework Foundation \
            -framework CoreFoundation \
            -framework Security

CXX_SOURCES := src/main.cpp src/util.cpp src/log.cpp src/config.cpp \
               src/http.cpp src/html.cpp src/portal.cpp src/keychain.cpp \
               src/dns.cpp src/netenv.cpp src/srun.cpp
MM_SOURCES  := src/wifi.mm

# Sources shared with the test binary (everything except main.cpp).
LIB_CXX_SOURCES := $(filter-out src/main.cpp,$(CXX_SOURCES))

OBJECTS := $(CXX_SOURCES:src/%.cpp=$(BUILD)/%.o) $(MM_SOURCES:src/%.mm=$(BUILD)/%.o)
LIB_OBJECTS := $(LIB_CXX_SOURCES:src/%.cpp=$(BUILD)/%.o) $(MM_SOURCES:src/%.mm=$(BUILD)/%.o)

TARGET      := $(BUILD)/schoolwifi
TEST_TARGET := $(BUILD)/schoolwifi_test

.PHONY: all clean install uninstall test e2e check run

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) $(LDFLAGS) -o $@
	@echo "built $@"

$(BUILD)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Objective-C++ for the CoreWLAN bridge. ARC is on so the ObjC objects in
# wifi.mm need no manual retain/release.
$(BUILD)/%.o: src/%.mm
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -fobjc-arc -c $< -o $@

$(TEST_TARGET): $(LIB_OBJECTS) tests/test_main.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) tests/test_main.cpp $(LIB_OBJECTS) $(LDFLAGS) -o $@

test: $(TEST_TARGET)
	@$(TEST_TARGET)

# Drives the real binary against tests/fake_portal.py: probe -> redirect ->
# meta refresh -> form -> POST -> verify. No campus network required.
e2e: $(TARGET)
	@./tests/e2e.sh

check: test e2e

run: $(TARGET)
	@$(TARGET) status

install: $(TARGET)
	install -d $(PREFIX)/bin
	install -m 0755 $(TARGET) $(PREFIX)/bin/schoolwifi
	@echo "installed $(PREFIX)/bin/schoolwifi"

uninstall:
	rm -f $(PREFIX)/bin/schoolwifi
	@echo "removed $(PREFIX)/bin/schoolwifi"

clean:
	rm -rf $(BUILD)
