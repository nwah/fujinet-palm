CC = m68k-palmos-gcc
CFLAGS = -O2 -Wall -palmos3.5

CORE_DIR   = ../../../core
PALM_DIR   = ../..
COMPAT_DIR = compat_include

# Order matters: core/'s fn_transport.h (once the core agent adds it) is
# found before the generated compat fallback, which is found before
# anything in palm/ itself.
INCLUDES = -I$(CORE_DIR) -I$(COMPAT_DIR) -I$(PALM_DIR)

fnlink.prc: fnlink fnlink.def bin.stamp
	build-prc -o $@ fnlink.def fnlink *.bin

fnlink: fnlink.c $(CORE_DIR)/fujibus.c $(CORE_DIR)/fn_fuji.c $(PALM_DIR)/transport_ser.c $(PALM_DIR)/transport_ser.h fnlink_rsc.h $(COMPAT_DIR)/fn_transport.h
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ fnlink.c $(CORE_DIR)/fujibus.c $(CORE_DIR)/fn_fuji.c $(PALM_DIR)/transport_ser.c

# Generated stand-in for core/fn_transport.h, used only until that file
# exists (see palm/fn_transport_compat.h). Regenerated on every build; the
# -I order above means it's simply never looked at once core/ provides the
# real header.
$(COMPAT_DIR)/fn_transport.h: $(PALM_DIR)/fn_transport_compat.h
	mkdir -p $(COMPAT_DIR)
	cp -f $(PALM_DIR)/fn_transport_compat.h $(COMPAT_DIR)/fn_transport.h

bin.stamp: fnlink.rcp fnlink_rsc.h
	pilrc -q fnlink.rcp
	touch $@

clean:
	rm -f fnlink fnlink.prc *.bin bin.stamp
	rm -rf $(COMPAT_DIR)
