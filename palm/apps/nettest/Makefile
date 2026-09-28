CC = m68k-palmos-gcc
CFLAGS = -O2 -Wall -palmos3.5 -I../../netshim

nettest.prc: nettest nettest.def bin.stamp
	build-prc -o $@ nettest.def nettest *.bin

nettest: nettest.c nettest_rsc.h ../../netshim/fnnetlib.h
	$(CC) $(CFLAGS) -o $@ nettest.c

bin.stamp: nettest.rcp nettest_rsc.h
	pilrc -q nettest.rcp
	touch $@

clean:
	rm -f nettest nettest.prc *.bin bin.stamp
