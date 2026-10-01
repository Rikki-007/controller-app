# Virtual controller daemon (vcd). GNU make; on Windows use mingw32-make.
#
#   make                  build build/stub/vcd (no driver needed, no real gamepad)
#   make BACKEND=vigem    Windows: real gamepad through ViGEmBus
#   make BACKEND=uinput   Linux: real gamepad through uinput
#   make test             build and run the unit tests (always the stub backend)
#   make clean
#
# Driver setup for each backend is described in docs/BUILD.md.

BACKEND   ?= stub
VIGEM_DIR ?= third_party/ViGEmClient

CC       ?= gcc
CXX      ?= g++
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
  native     = $(subst /,\,$1)
  MKDIR      = if not exist $(call native,$1) mkdir $(call native,$1)
  RMDIR      = if exist $(call native,$1) rmdir /s /q $(call native,$1)
else
  EXE       :=
  NET_LIBS  :=
  native     = $1
  MKDIR      = mkdir -p $1
  RMDIR      = rm -rf $1
endif

# ---- Backend ---------------------------------------------------------------

OUT := build/$(BACKEND)

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
OBJS    := $(OUT)/main.o $(OUT)/network.o $(OUT)/parser.o $(OUT)/hid_output.o
VCD     := $(OUT)/vcd$(EXE)

.PHONY: all test clean
all: $(VCD)

$(VCD): $(OBJS) $(HID_OBJS)
	$(CC) $(LDFLAGS) $^ -o $@ $(HID_LIBS) $(NET_LIBS)

# Objects live in a per-backend directory, so switching BACKEND never links
# objects built for another one.
$(OUT)/%.o: src/%.c $(HEADERS) | $(OUT)
	$(CC) $(WARNINGS) $(CFLAGS) $(CPPFLAGS) $(HID_CPPFLAGS) -c $< -o $@

$(OUT)/ViGEmClient.o: $(VIGEM_DIR)/src/ViGEmClient.cpp | $(OUT)
	$(CXX) $(CFLAGS) -I$(VIGEM_DIR)/include -I$(VIGEM_DIR)/src -c $< -o $@

# ---- Tests -----------------------------------------------------------------

TEST_OUT  := build/test
TEST_BINS := $(TEST_OUT)/test_parser$(EXE) $(TEST_OUT)/test_hid_output$(EXE) \
             $(TEST_OUT)/test_network$(EXE)
TEST_DEPS := $(HEADERS) tests/test_util.h
TEST_CC    = $(CC) $(WARNINGS) $(CFLAGS) $(CPPFLAGS) -Itests

test: $(TEST_BINS)
	$(call native,$(TEST_OUT)/test_parser$(EXE))
	$(call native,$(TEST_OUT)/test_hid_output$(EXE))
	$(call native,$(TEST_OUT)/test_network$(EXE))

$(TEST_OUT)/test_parser$(EXE): tests/test_parser.c src/parser.c $(TEST_DEPS) | $(TEST_OUT)
	$(TEST_CC) tests/test_parser.c src/parser.c -o $@

$(TEST_OUT)/test_hid_output$(EXE): tests/test_hid_output.c src/hid_output.c $(TEST_DEPS) | $(TEST_OUT)
	$(TEST_CC) tests/test_hid_output.c src/hid_output.c -o $@

$(TEST_OUT)/test_network$(EXE): tests/test_network.c src/network.c src/parser.c $(TEST_DEPS) | $(TEST_OUT)
	$(TEST_CC) tests/test_network.c src/network.c src/parser.c -o $@ $(NET_LIBS)

# ---- Housekeeping ----------------------------------------------------------

$(OUT) $(TEST_OUT):
	$(call MKDIR,$@)

clean:
	$(call RMDIR,build)
