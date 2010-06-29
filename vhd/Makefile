BLKTAP_ROOT := ../
include $(BLKTAP_ROOT)/Rules.mk

IBIN               = vhd-util vhd-index vhd-update
INST_DIR           = /usr/sbin

CFLAGS            += -Werror
CFLAGS            += -Wno-unused
CFLAGS            += -I../include
CFLAGS            += -D_GNU_SOURCE

ifeq ($(VHD_STATIC),y)
CFLAGS            += -static
endif

LIBS              := -Llib -lvhd
LIBS              += -luuid

# Get gcc to generate the dependencies for us.
CFLAGS            += -Wp,-MD,.$(@F).d
DEPS               = .*.d

all: build

build: libvhd $(IBIN)

libvhd:
	@set -e
	$(MAKE) -C lib all

vhd-util: vhd-util.o
	$(CC) $(CFLAGS) -o vhd-util vhd-util.o $(LIBS)

vhd-index: vhd-index.o
	$(CC) $(CFLAGS) -o vhd-index vhd-index.o $(LIBS)

vhd-update: vhd-update.o
	$(CC) $(CFLAGS) -o vhd-update vhd-update.o $(LIBS)

install: all
	$(MAKE) -C lib install
	$(INSTALL_DIR) -p $(DESTDIR)$(INST_DIR)
	$(INSTALL_PROG) $(IBIN) $(DESTDIR)$(INST_DIR)

clean:
	$(MAKE) -C lib clean
	rm -rf *.o *~ $(DEPS) $(IBIN)

.PHONY: all build clean install libvhd

-include $(DEPS)
