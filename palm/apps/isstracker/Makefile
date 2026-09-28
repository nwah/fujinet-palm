CC = m68k-palmos-gcc
CFLAGS = -O2 -Wall -palmos3.5

FNLIB ?= ../../../../fujinet-lib-palmos
COMMON = ../common
INCLUDES = -I$(FNLIB)/include -I$(COMMON)
LIBDIR = $(FNLIB)/r2r/palmos
LIBS = -L$(LIBDIR) -lfujinet.palmos

isstracker.prc: isstracker isstracker.def bin.stamp
	build-prc -o $@ isstracker.def isstracker *.bin

isstracker: isstracker.c isstracker_rsc.h $(COMMON)/fnapp.c $(COMMON)/fnapp.h $(LIBDIR)/libfujinet.palmos.a
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ isstracker.c $(COMMON)/fnapp.c $(LIBS)

$(LIBDIR)/libfujinet.palmos.a:
	$(MAKE) -C $(FNLIB) palmos

bin.stamp: isstracker.rcp isstracker_rsc.h map.bmp
	pilrc -q isstracker.rcp
	touch $@

clean:
	rm -f isstracker isstracker.prc *.bin bin.stamp
