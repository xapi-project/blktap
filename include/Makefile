BLKTAP_ROOT := ..
include $(BLKTAP_ROOT)/Rules.mk

.PHONY: all
all:

.PHONY: install
install:
	$(INSTALL_DIR) -p $(DESTDIR)/usr/include
	$(INSTALL_DATA) blktaplib.h $(DESTDIR)/usr/include
