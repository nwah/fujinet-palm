CC = m68k-palmos-gcc
CFLAGS = -O2 -Wall -palmos3.5

FNLIB ?= ../../../../fujinet-lib-palmos
INCLUDES = -I$(FNLIB)/include
LIBDIR = $(FNLIB)/r2r/palmos
LIBS = -L$(LIBDIR) -lfujinet.palmos

fujiconfig.prc: fujiconfig fujiconfig.def bin.stamp
	build-prc -o $@ fujiconfig.def fujiconfig *.bin

fujiconfig: fujiconfig.c fujiconfig_rsc.h $(LIBDIR)/libfujinet.palmos.a
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ fujiconfig.c $(LIBS)

$(LIBDIR)/libfujinet.palmos.a:
	$(MAKE) -C $(FNLIB) palmos

bin.stamp: fujiconfig.rcp fujiconfig_rsc.h
	pilrc -q fujiconfig.rcp
	touch $@

clean:
	rm -f fujiconfig fujiconfig.prc *.bin bin.stamp
