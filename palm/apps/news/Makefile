CC = m68k-palmos-gcc
CFLAGS = -O2 -Wall -palmos3.5

FNLIB ?= ../../../../fujinet-lib-palmos
COMMON = ../common
MASTODON = ../mastodon
INCLUDES = -I$(FNLIB)/include -I$(COMMON)
LIBDIR = $(FNLIB)/r2r/palmos
LIBS = -L$(LIBDIR) -lfujinet.palmos

news.prc: news news.def bin.stamp
	build-prc -o $@ news.def news *.bin

news: news.c news_rsc.h $(COMMON)/fnapp.c $(COMMON)/fnapp.h $(MASTODON)/htmlclean.c $(MASTODON)/htmlclean.h $(LIBDIR)/libfujinet.palmos.a
	$(CC) $(CFLAGS) $(INCLUDES) -I$(MASTODON) -o $@ news.c $(COMMON)/fnapp.c $(MASTODON)/htmlclean.c $(LIBS)

$(LIBDIR)/libfujinet.palmos.a:
	$(MAKE) -C $(FNLIB) palmos

bin.stamp: news.rcp news_rsc.h
	pilrc -q news.rcp
	touch $@

clean:
	rm -f news news.prc *.bin bin.stamp
