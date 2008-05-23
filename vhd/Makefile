BLKTAP_ROOT := ../
include $(BLKTAP_ROOT)/Rules.mk

IBIN               = vhd-util
INST_DIR           = /usr/sbin

LIBDIR             = lib

CFLAGS            += -Werror
CFLAGS            += -Wno-unused
CFLAGS            += -I../include
CFLAGS            += -D_GNU_SOURCE

ifeq ($(VHD_STATIC),y)
CFLAGS            += -static
endif

LIBS              := -L$(LIBDIR) -lvhd
LIBS              += -luuid

# Get gcc to generate the dependencies for us.
CFLAGS            += -Wp,-MD,.$(@F).d
DEPS               = .*.d

all: build

build: libvhd $(IBIN)

libvhd:
	@set -e
	$(MAKE) -C $(LIBDIR) all

vhd-util: vhd-util.o
	$(CC) $(CFLAGS) -o vhd-util vhd-util.o $(LIBS)

install: all
	$(MAKE) -C $(LIBDIR) install
	$(INSTALL_DIR) -p $(DESTDIR)$(INST_DIR)
	$(INSTALL_PROG) $(IBIN) $(DESTDIR)$(INST_DIR)

clean:
	$(MAKE) -C $(LIBDIR) clean
	rm -rf *.o *~ $(DEPS) $(IBIN)

.PHONY: all build clean install libvhd

-include $(DEPS)
