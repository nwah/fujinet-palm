CC = m68k-palmos-gcc
CFLAGS = -O2 -Wall -palmos3.5

hello.prc: hello hello.def bin.stamp
	build-prc -o $@ hello.def hello *.bin

hello: hello.c hello_rsc.h
	$(CC) $(CFLAGS) -o $@ hello.c

bin.stamp: hello.rcp hello_rsc.h
	pilrc -q hello.rcp
	touch $@

clean:
	rm -f hello hello.prc *.bin bin.stamp
