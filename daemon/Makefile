BLKTAP_ROOT := ..
include $(BLKTAP_ROOT)/Rules.mk

IBIN          = blktapctrl tapdisk-control
INST_DIR      = /usr/sbin

LIBDIR        = lib

LIBS         := -lxenstore
LIBS         += -Llib
LIBS         += -lblktap
LIBS         += -lxenctrl

ifneq ($(USE_SYSTEM_LIBRARIES),y)
INCLUDES     += -I $(XEN_LIBXC) -I $(XEN_XENSTORE)
LIBS         += -L$(XEN_XENSTORE)
endif

SHARED-OBJS  := tapdisk-dispatch-common.o

CONTROL-OBJS := tapdisk-xenbus.o
CONTROL-OBJS += tapdisk-blkif.o

CFLAGS       += -Werror
CFLAGS       += -Wno-unused
CFLAGS       += -fno-strict-aliasing -fPIC
CFLAGS       += -Ilib -I../include -I../drivers
CFLAGS       += -D_GNU_SOURCE
CFLAGS       += -g

# Get gcc to generate the dependencies for us.
CFLAGS       += -Wp,-MD,.$(@F).d
DEPS          = .*.d

all: libblktap $(IBIN)

blktapctrl: tapdisk-daemon.c $(SHARED-OBJS)
	$(CC) $(CFLAGS) -o blktapctrl tapdisk-daemon.c $(LIBS) $(SHARED-OBJS)

tapdisk-control: tapdisk-control.c $(SHARED-OBJS) $(CONTROL-OBJS)
	$(CC) $(CFLAGS) -o tapdisk-control tapdisk-control.c \
		$(LIBS) $(SHARED-OBJS) $(CONTROL-OBJS)

libblktap:
	@set -e
	$(MAKE) -C $(LIBDIR) all

install: all
	$(MAKE) -C $(LIBDIR) install
	$(INSTALL_DIR) -p $(DESTDIR)$(INST_DIR)
	$(INSTALL_PROG) $(IBIN) $(DESTDIR)$(INST_DIR)

clean:
	$(MAKE) -C $(LIBDIR) clean
	rm -rf *.o *~ $(DEPS) xen TAGS

.PHONY: all clean install blktapctrl tapdisk-control libblktap

-include $(DEPS)

