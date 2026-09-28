# Makefile for fnnetlib.prc: the "FujiNet NetLib" system library ('libr',
# creator 'FnNL') that splices a FujiBus-backed emulation table into the
# ROM Net.lib so existing NetLib apps work unmodified over a FujiNet link.
#
# Built as a system library via `build-prc -L`, which (verified separately,
# see docker/_libtest experiments in the task report) cannot take a .def
# file at all ("-l/-L/-H/--hack options conflict with definition file") --
# name/creator/type are passed as plain build-prc flags instead of through
# a .def like the app Makefiles in this tree use.
#
# -nostartfiles is essential: a system library gets NO A5 globals page (see
# fnnl_priv.h), and the normal Palm OS crt0 startup code both expects one
# and defines a PilotMain-calling `start` that would collide with our own
# entry trampoline (entry.s) anyway -- the linker must not pull in
# /usr/m68k-palmos/lib/crt0.o at all.

CC = m68k-palmos-gcc

CORE_DIR = ../../core
PALM_DIR = ..

INCLUDES = -I$(CORE_DIR) -I$(PALM_DIR) -I.
CFLAGS   = -O2 -Wall -palmos3.5 -nostartfiles $(INCLUDES)

OBJS = entry.o fnnl_glob.o fnnl_own.o fnnl_net.o fnnl_sock.o fnnl_io.o \
       fnnl_dns.o fnnl_if.o fnnl_bits.o fujibus.o fn_net.o transport_ser.o

HDRS = fnnetlib.h fnnl_priv.h

fnnetlib.prc: fnnetlib
	build-prc -L -c FnNL -n "FujiNet NetLib" -o $@ fnnetlib

fnnetlib: $(OBJS)
	$(CC) -nostartfiles -o $@ $(OBJS)

entry.o: entry.s
	$(CC) $(CFLAGS) -c entry.s -o entry.o

fnnl_glob.o: fnnl_glob.c $(HDRS)
	$(CC) $(CFLAGS) -c fnnl_glob.c -o fnnl_glob.o

fnnl_own.o: fnnl_own.c $(HDRS)
	$(CC) $(CFLAGS) -c fnnl_own.c -o fnnl_own.o

fnnl_net.o: fnnl_net.c $(HDRS)
	$(CC) $(CFLAGS) -c fnnl_net.c -o fnnl_net.o

fnnl_sock.o: fnnl_sock.c $(HDRS)
	$(CC) $(CFLAGS) -c fnnl_sock.c -o fnnl_sock.o

fnnl_io.o: fnnl_io.c $(HDRS)
	$(CC) $(CFLAGS) -c fnnl_io.c -o fnnl_io.o

fnnl_dns.o: fnnl_dns.c $(HDRS)
	$(CC) $(CFLAGS) -c fnnl_dns.c -o fnnl_dns.o

fnnl_if.o: fnnl_if.c $(HDRS)
	$(CC) $(CFLAGS) -c fnnl_if.c -o fnnl_if.o

fnnl_bits.o: fnnl_bits.c $(HDRS)
	$(CC) $(CFLAGS) -c fnnl_bits.c -o fnnl_bits.o

fujibus.o: $(CORE_DIR)/fujibus.c
	$(CC) $(CFLAGS) -c $(CORE_DIR)/fujibus.c -o fujibus.o

fn_net.o: $(CORE_DIR)/fn_net.c
	$(CC) $(CFLAGS) -c $(CORE_DIR)/fn_net.c -o fn_net.o

transport_ser.o: $(PALM_DIR)/transport_ser.c $(PALM_DIR)/transport_ser.h
	$(CC) $(CFLAGS) -c $(PALM_DIR)/transport_ser.c -o transport_ser.o

clean:
	rm -f *.o fnnetlib fnnetlib.prc
