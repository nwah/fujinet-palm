CC = m68k-palmos-gcc
CFLAGS = -O2 -Wall -palmos3.5

CORE_DIR   = ../../../core
PALM_DIR   = ../..
COMPAT_DIR = compat_include

# Order matters: core/'s fn_transport.h is found before the generated compat
# fallback, which is found before anything in palm/ itself. Mirrors
# ../fnlink/Makefile.
INCLUDES = -I$(CORE_DIR) -I$(COMPAT_DIR) -I$(PALM_DIR)

mastodon.prc: mastodon mastodon.def bin.stamp
	build-prc -o $@ mastodon.def mastodon *.bin

mastodon: mastodon.c htmlclean.c htmlclean.h $(CORE_DIR)/fujibus.c $(CORE_DIR)/fn_net.c $(PALM_DIR)/transport_ser.c $(PALM_DIR)/transport_ser.h mastodon_rsc.h $(COMPAT_DIR)/fn_transport.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ mastodon.c htmlclean.c $(CORE_DIR)/fujibus.c $(CORE_DIR)/fn_net.c $(PALM_DIR)/transport_ser.c

# Generated stand-in for core/fn_transport.h, used only until that file
# exists (see palm/fn_transport_compat.h). Regenerated on every build; the
# -I order above means it's simply never looked at once core/ provides the
# real header.
$(COMPAT_DIR)/fn_transport.h: $(PALM_DIR)/fn_transport_compat.h
	mkdir -p $(COMPAT_DIR)
	cp -f $(PALM_DIR)/fn_transport_compat.h $(COMPAT_DIR)/fn_transport.h

bin.stamp: mastodon.rcp mastodon_rsc.h
	pilrc -q mastodon.rcp
	touch $@

clean:
	rm -f mastodon mastodon.prc *.bin bin.stamp
	rm -rf $(COMPAT_DIR)
