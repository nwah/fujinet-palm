CC = m68k-palmos-gcc
CFLAGS = -O2 -Wall -palmos3.5

FNLIB ?= ../../../../fujinet-lib-palmos
COMMON = ../common
INCLUDES = -I$(FNLIB)/include -I$(COMMON)
LIBDIR = $(FNLIB)/r2r/palmos
LIBS = -L$(LIBDIR) -lfujinet.palmos

mastodon.prc: mastodon mastodon.def bin.stamp
	build-prc -o $@ mastodon.def mastodon *.bin

mastodon: mastodon.c htmlclean.c htmlclean.h mastodon_rsc.h $(COMMON)/fnapp.c $(COMMON)/fnapp.h $(LIBDIR)/libfujinet.palmos.a
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ mastodon.c htmlclean.c $(COMMON)/fnapp.c $(LIBS)

$(LIBDIR)/libfujinet.palmos.a:
	$(MAKE) -C $(FNLIB) palmos

bin.stamp: mastodon.rcp mastodon_rsc.h
	pilrc -q mastodon.rcp
	touch $@

clean:
	rm -f mastodon mastodon.prc *.bin bin.stamp
