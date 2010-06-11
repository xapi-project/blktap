BLKTAP_ROOT := ..
include $(BLKTAP_ROOT)/Rules.mk

.PHONY: all
all:

.PHONY: install
install:
	$(INSTALL_DIR) -p $(DESTDIR)/usr/include/blktap
	$(INSTALL_DATA) blktaplib.h $(DESTDIR)/usr/include/blktap
	$(INSTALL_DATA) tapdisk-message.h $(DESTDIR)/usr/include/blktap

	$(INSTALL_DIR) -p $(DESTDIR)/usr/include/vhd
	$(INSTALL_DATA) vhd.h $(DESTDIR)/usr/include/vhd
	$(INSTALL_DATA) vhd-util.h $(DESTDIR)/usr/include/vhd
	$(INSTALL_DATA) libvhd.h $(DESTDIR)/usr/include/vhd
	$(INSTALL_DATA) libvhd-index.h $(DESTDIR)/usr/include/vhd
	$(INSTALL_DATA) libvhd-journal.h $(DESTDIR)/usr/include/vhd
	$(INSTALL_DATA) list.h $(DESTDIR)/usr/include/vhd

.PHONY: clean
clean:
	@:
