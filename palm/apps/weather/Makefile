CC = m68k-palmos-gcc
CFLAGS = -O2 -Wall -palmos3.5

FNLIB ?= ../../../../fujinet-lib-palmos
COMMON = ../common
INCLUDES = -I$(FNLIB)/include -I$(COMMON)
LIBDIR = $(FNLIB)/r2r/palmos
LIBS = -L$(LIBDIR) -lfujinet.palmos

weather.prc: weather weather.def bin.stamp
	build-prc -o $@ weather.def weather *.bin

weather: weather.c weather_rsc.h $(COMMON)/fnapp.c $(COMMON)/fnapp.h $(LIBDIR)/libfujinet.palmos.a
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ weather.c $(COMMON)/fnapp.c $(LIBS)

$(LIBDIR)/libfujinet.palmos.a:
	$(MAKE) -C $(FNLIB) palmos

bin.stamp: weather.rcp weather_rsc.h icons/*.pbitm
	pilrc -q weather.rcp
	touch $@

clean:
	rm -f weather weather.prc *.bin bin.stamp
