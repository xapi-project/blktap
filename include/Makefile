BLKTAP_ROOT := ..
include $(BLKTAP_ROOT)/Rules.mk

LIBVHDDIR      := vhd
LIBVHD_HEADERS := vhd.h libvhd.h libvhd-index.h libvhd-journal.h list.h

BLKTAPDIR      := blktap
BLKTAP_HEADERS := blktap2.h blktaplib.h tapdisk-message.h tap-ctl.h

.PHONY: all
all:

.PHONY: install
install:
	$(INSTALL_DIR) -p $(DESTDIR)/usr/include/$(BLKTAPDIR)
	$(INSTALL_DATA) $(BLKTAP_HEADERS) $(DESTDIR)/usr/include/$(BLKTAPDIR)
	$(INSTALL_DIR) -p $(DESTDIR)/usr/include/$(LIBVHDDIR)
	$(INSTALL_DATA) $(LIBVHD_HEADERS) $(DESTDIR)/usr/include/$(LIBVHDDIR)

.PHONY: clean
clean:
	@:
