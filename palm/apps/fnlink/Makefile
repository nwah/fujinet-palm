CC = m68k-palmos-gcc
CFLAGS = -O2 -Wall -palmos3.5

# fujinet-lib: the public API for the Fuji test, and its bus/palmos serial
# transport (transport_ser.h, fn_ser_*) for the raw echo test
FNLIB ?= ../../../../fujinet-lib-palmos
INCLUDES = -I$(FNLIB)/include -I$(FNLIB)/bus/palmos
LIBDIR = $(FNLIB)/r2r/palmos
LIBS = -L$(LIBDIR) -lfujinet.palmos

fnlink.prc: fnlink fnlink.def bin.stamp
	build-prc -o $@ fnlink.def fnlink *.bin

fnlink: fnlink.c fnlink_rsc.h $(LIBDIR)/libfujinet.palmos.a
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ fnlink.c $(LIBS)

$(LIBDIR)/libfujinet.palmos.a:
	$(MAKE) -C $(FNLIB) palmos

bin.stamp: fnlink.rcp fnlink_rsc.h
	pilrc -q fnlink.rcp
	touch $@

clean:
	rm -f fnlink fnlink.prc *.bin bin.stamp
