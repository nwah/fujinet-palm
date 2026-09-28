# Makefile for the FujiBus macOS host CLI (fnhost) and its unit tests.
#
# Core library sources (../core/*.c) are portable ANSI C89 and are compiled
# strictly to that standard. Host sources (this directory + tests/) are
# ordinary C99 host code and may use libc/POSIX freely.

CC ?= cc

# Both CORE_CFLAGS and HOST_CFLAGS define FN_HAVE_STDINT: every translation
# unit that includes core/fn_types.h (directly or via fujibus.h) must agree
# on fn_u8/fn_u16/fn_u32's underlying types, or FnCtx/FnParams (which contain
# fn_u32 fields) get a different size/layout in host .o files than in the
# core .o files they link against -- fn_u32 as "unsigned long" is 8 bytes on
# 64-bit macOS/Linux but uint32_t is always 4, so without this the struct
# layouts silently disagree across the host/core boundary (return codes and
# separately-passed output buffers happen to still come out right, but any
# code that reads FnCtx fields directly -- e.g. the unit tests -- silently
# reads garbage).
CORE_CFLAGS = -std=c89 -pedantic -Wall -Wextra -Werror -DFN_HAVE_STDINT -I../core
HOST_CFLAGS = -std=c99 -Wall -Wextra -DFN_HAVE_STDINT -I../core

BUILD = build

CORE_OBJS = $(BUILD)/fujibus.o $(BUILD)/fn_fuji.o $(BUILD)/fn_net.o
HOST_OBJS = $(BUILD)/transport_tcp.o $(BUILD)/transport_serial.o $(BUILD)/fnhost.o
TEST_OBJS = $(BUILD)/test_core.o

.PHONY: all test clean

all: fnhost tests/test_core

fnhost: $(CORE_OBJS) $(HOST_OBJS)
	$(CC) $(HOST_CFLAGS) -o fnhost $(CORE_OBJS) $(HOST_OBJS)

tests/test_core: $(CORE_OBJS) $(TEST_OBJS)
	$(CC) $(HOST_CFLAGS) -o tests/test_core $(CORE_OBJS) $(TEST_OBJS)

test: tests/test_core
	./tests/test_core

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/fujibus.o: ../core/fujibus.c | $(BUILD)
	$(CC) $(CORE_CFLAGS) -c ../core/fujibus.c -o $(BUILD)/fujibus.o

$(BUILD)/fn_fuji.o: ../core/fn_fuji.c | $(BUILD)
	$(CC) $(CORE_CFLAGS) -c ../core/fn_fuji.c -o $(BUILD)/fn_fuji.o

$(BUILD)/fn_net.o: ../core/fn_net.c | $(BUILD)
	$(CC) $(CORE_CFLAGS) -c ../core/fn_net.c -o $(BUILD)/fn_net.o

$(BUILD)/transport_tcp.o: transport_tcp.c transport_tcp.h | $(BUILD)
	$(CC) $(HOST_CFLAGS) -c transport_tcp.c -o $(BUILD)/transport_tcp.o

$(BUILD)/transport_serial.o: transport_serial.c transport_serial.h | $(BUILD)
	$(CC) $(HOST_CFLAGS) -c transport_serial.c -o $(BUILD)/transport_serial.o

$(BUILD)/fnhost.o: fnhost.c transport_tcp.h transport_serial.h | $(BUILD)
	$(CC) $(HOST_CFLAGS) -c fnhost.c -o $(BUILD)/fnhost.o

$(BUILD)/test_core.o: tests/test_core.c | $(BUILD)
	$(CC) $(HOST_CFLAGS) -c tests/test_core.c -o $(BUILD)/test_core.o

clean:
	rm -rf $(BUILD) fnhost tests/test_core
