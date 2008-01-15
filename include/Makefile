XEN_ROOT = ../../..
include $(XEN_ROOT)/tools/Rules.mk

.PHONY: all
all:

.PHONY: install
install:
	$(INSTALL_DIR) -p $(DESTDIR)/usr/include
	$(INSTALL_PROG) blktaplib.h $(DESTDIR)/usr/include
