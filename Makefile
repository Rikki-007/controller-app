# Virtual controller daemon (vcd). GNU make; on Windows use mingw32-make.
#
#   make                  build build/stub/vcd (no driver needed, no real gamepad)
#                         and the test client build/tools/vc_send
#   make BACKEND=vigem    Windows: real gamepad through ViGEmBus
#   make BACKEND=uinput   Linux: real gamepad through uinput
#   make test             build and run the unit tests (always the stub backend)
#   make test-build       build the unit tests without running them
#   make e2e              run vcd and drive it with vc_send (needs sh)
#   make clean
#
# Driver setup for each backend is described in docs/BUILD.md. Windows with
# Smart App Control on blocks freshly built binaries: use scripts/test.ps1
# there instead of make test (see docs/BUILD.md).

BACKEND   ?= stub
VIGEM_DIR ?= third_party/ViGEmClient
# Output root. Give each OS its own when they share a checkout (Windows and
# WSL), so neither links the other's objects: make BUILD=build/wsl test
BUILD     ?= build

# CC and CXX always have make's built-in defaults (cc, g++), so ?= would never
# apply. Replace only those defaults; CC=... from the command line or the
# environment still wins.
ifeq ($(origin CC),default)
  CC := gcc
endif
ifeq ($(origin CXX),default)
  CXX := g++
endif
CFLAGS   ?= -O2
WARNINGS := -std=c99 -pedantic -Wall -Wextra -Werror
CPPFLAGS := -Iinclude

# ---- Platform --------------------------------------------------------------

ifeq ($(OS),Windows_NT)
  # Use cmd.exe whatever shell make was started from, so the recipes below
  # behave the same in PowerShell, cmd and MSYS.
  SHELL     := cmd.exe
  EXE       := .exe
  NET_LIBS  := -lws2_32
  VCD_LIBS  := -lws2_32 -lbcrypt
  TOOL_LIBS := -lws2_32 -lwinmm
  native     = $(subst /,\,$1)
  MKDIR      = if not exist $(call native,$1) mkdir $(call native,$1)
  RMDIR      = if exist $(call native,$1) rmdir /s /q $(call native,$1)
else
  EXE       :=
  NET_LIBS  :=
  VCD_LIBS  :=
  TOOL_LIBS :=
  native     = $1
  MKDIR      = mkdir -p $1
  RMDIR      = rm -rf $1
endif

# ---- Backend ---------------------------------------------------------------

OUT := $(BUILD)/$(BACKEND)

ifeq ($(BACKEND),stub)
  HID_CPPFLAGS :=
  HID_OBJS     :=
  HID_LIBS     :=
else ifeq ($(BACKEND),vigem)
  ifeq ($(wildcard $(VIGEM_DIR)/include/ViGEm/Client.h),)
    $(error ViGEmClient not found in '$(VIGEM_DIR)'. Run: git clone https://github.com/nefarius/ViGEmClient.git $(VIGEM_DIR)   (or pass VIGEM_DIR=path))
  endif
  # -isystem: the ViGEm headers produce warnings of their own under -Wextra.
  HID_CPPFLAGS := -DVC_HID_BACKEND_VIGEM -isystem $(VIGEM_DIR)/include
  HID_OBJS     := $(OUT)/ViGEmClient.o
  HID_LIBS     := -lsetupapi -lstdc++
else ifeq ($(BACKEND),uinput)
  HID_CPPFLAGS := -DVC_HID_BACKEND_UINPUT
  HID_OBJS     :=
  HID_LIBS     :=
else
  $(error BACKEND must be stub, vigem or uinput (got '$(BACKEND)'))
endif

# ---- Daemon ----------------------------------------------------------------

HEADERS := $(wildcard include/*.h)
OBJS    := $(OUT)/main.o $(OUT)/network.o $(OUT)/parser.o $(OUT)/hid_output.o \
           $(OUT)/protocol.o $(OUT)/session.o
VCD     := $(OUT)/vcd$(EXE)

# The test client does not depend on the backend, so all backends share it.
TOOLS_OUT := $(BUILD)/tools
VC_SEND   := $(TOOLS_OUT)/vc_send$(EXE)

.PHONY: all test test-build e2e clean
all: $(VCD) $(VC_SEND)

$(VCD): $(OBJS) $(HID_OBJS)
	$(CC) $(LDFLAGS) $^ -o $@ $(HID_LIBS) $(VCD_LIBS)

# Objects live in a per-backend directory, so switching BACKEND never links
# objects built for another one.
$(OUT)/%.o: src/%.c $(HEADERS) | $(OUT)
	$(CC) $(WARNINGS) $(CFLAGS) $(CPPFLAGS) $(HID_CPPFLAGS) -c $< -o $@

$(OUT)/ViGEmClient.o: $(VIGEM_DIR)/src/ViGEmClient.cpp | $(OUT)
	$(CXX) $(CFLAGS) -I$(VIGEM_DIR)/include -I$(VIGEM_DIR)/src -c $< -o $@

# ---- Tools -----------------------------------------------------------------

$(VC_SEND): tools/vc_send.c src/encoder.c src/protocol.c $(HEADERS) | $(TOOLS_OUT)
	$(CC) $(WARNINGS) $(CFLAGS) $(CPPFLAGS) tools/vc_send.c src/encoder.c src/protocol.c -o $@ $(TOOL_LIBS)

# ---- Tests -----------------------------------------------------------------

TEST_OUT  := $(BUILD)/test
TEST_BINS := $(TEST_OUT)/test_parser$(EXE) $(TEST_OUT)/test_hid_output$(EXE) \
             $(TEST_OUT)/test_network$(EXE) $(TEST_OUT)/test_encoder$(EXE) \
             $(TEST_OUT)/test_loopback$(EXE) $(TEST_OUT)/test_protocol$(EXE) \
             $(TEST_OUT)/test_session$(EXE) $(TEST_OUT)/test_layout$(EXE)
TEST_DEPS := $(HEADERS) tests/test_util.h
TEST_CC    = $(CC) $(WARNINGS) $(CFLAGS) $(CPPFLAGS) -Itests

# The phone-side engine (src/vc_client.c, shared by the iOS and Android apps)
# is POSIX + C11, so its test is built everywhere but Windows.
ifneq ($(OS),Windows_NT)
  CLIENT_TEST := $(TEST_OUT)/test_client
  TEST_BINS   += $(CLIENT_TEST)
endif

test-build: $(TEST_BINS)

test: $(TEST_BINS)
	$(call native,$(TEST_OUT)/test_parser$(EXE))
	$(call native,$(TEST_OUT)/test_hid_output$(EXE))
	$(call native,$(TEST_OUT)/test_network$(EXE))
	$(call native,$(TEST_OUT)/test_encoder$(EXE))
	$(call native,$(TEST_OUT)/test_loopback$(EXE))
	$(call native,$(TEST_OUT)/test_protocol$(EXE))
	$(call native,$(TEST_OUT)/test_session$(EXE))
	$(call native,$(TEST_OUT)/test_layout$(EXE))
	$(CLIENT_TEST)

e2e: $(VCD) $(VC_SEND)
	sh tests/e2e.sh $(VCD) $(VC_SEND)

$(TEST_OUT)/test_parser$(EXE): tests/test_parser.c src/parser.c $(TEST_DEPS) | $(TEST_OUT)
	$(TEST_CC) tests/test_parser.c src/parser.c -o $@

$(TEST_OUT)/test_hid_output$(EXE): tests/test_hid_output.c src/hid_output.c $(TEST_DEPS) | $(TEST_OUT)
	$(TEST_CC) tests/test_hid_output.c src/hid_output.c -o $@

$(TEST_OUT)/test_network$(EXE): tests/test_network.c src/network.c src/parser.c $(TEST_DEPS) | $(TEST_OUT)
	$(TEST_CC) tests/test_network.c src/network.c src/parser.c -o $@ $(NET_LIBS)

$(TEST_OUT)/test_encoder$(EXE): tests/test_encoder.c src/encoder.c src/parser.c $(TEST_DEPS) | $(TEST_OUT)
	$(TEST_CC) tests/test_encoder.c src/encoder.c src/parser.c -o $@

$(TEST_OUT)/test_loopback$(EXE): tests/test_loopback.c src/encoder.c src/network.c src/parser.c src/hid_output.c $(TEST_DEPS) | $(TEST_OUT)
	$(TEST_CC) tests/test_loopback.c src/encoder.c src/network.c src/parser.c src/hid_output.c -o $@ $(NET_LIBS)

$(TEST_OUT)/test_protocol$(EXE): tests/test_protocol.c src/protocol.c $(TEST_DEPS) | $(TEST_OUT)
	$(TEST_CC) tests/test_protocol.c src/protocol.c -o $@

$(TEST_OUT)/test_session$(EXE): tests/test_session.c src/session.c src/protocol.c src/parser.c src/hid_output.c $(TEST_DEPS) | $(TEST_OUT)
	$(TEST_CC) tests/test_session.c src/session.c src/protocol.c src/parser.c src/hid_output.c -o $@

$(TEST_OUT)/test_layout$(EXE): tests/test_layout.c src/layout.c src/parser.c $(TEST_DEPS) | $(TEST_OUT)
	$(TEST_CC) tests/test_layout.c src/layout.c src/parser.c -o $@ -lm

# -std=c11 after $(WARNINGS)'s -std=c99: the last one wins.
CLIENT_TEST_SRCS := tests/test_client.c src/vc_client.c src/encoder.c src/protocol.c \
                    src/network.c src/session.c src/parser.c src/hid_output.c
$(TEST_OUT)/test_client: $(CLIENT_TEST_SRCS) $(TEST_DEPS) | $(TEST_OUT)
	$(TEST_CC) -std=c11 $(CLIENT_TEST_SRCS) -o $@ -pthread

# ---- Housekeeping ----------------------------------------------------------

$(OUT) $(TEST_OUT) $(TOOLS_OUT):
	$(call MKDIR,$@)

clean:
	$(call RMDIR,$(BUILD))
