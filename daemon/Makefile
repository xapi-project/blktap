BLKTAP_ROOT := ..
include $(BLKTAP_ROOT)/Rules.mk

IBIN          = blktapctrl
INST_DIR      = /usr/sbin

LIBS         := -lxenstore
LIBS         += -Llib
LIBS         += -lblktap
LIBS         += -lxenctrl

ifneq ($(USE_SYSTEM_LIBRARIES),y)
INCLUDES     += -I $(XEN_LIBXC) -I $(XEN_XENSTORE) -I $(XEN_ROOT)/tools/include
LIBS         += -L $(XEN_LIBXC) -L $(XEN_XENSTORE)
endif

OBJS         := tapdisk-dispatch-common.o
OBJS         += tapdisk-channel.o

CFLAGS       += -Werror
CFLAGS       += -Wno-unused
CFLAGS       += -fno-strict-aliasing -fPIC
CFLAGS       += -Ilib -I../include -I../drivers $(INCLUDES)
CFLAGS       += -D_GNU_SOURCE
CFLAGS       += -g

# Get gcc to generate the dependencies for us.
CFLAGS       += -Wp,-MD,.$(@F).d
DEPS          = .*.d

all: libblktap $(IBIN)

blktapctrl: tapdisk-daemon.c $(OBJS)
	$(CC) $(CFLAGS) -o blktapctrl tapdisk-daemon.c $(LIBS) $(OBJS)

libblktap:
	@set -e
	$(MAKE) -C lib all

install: all
	$(MAKE) -C lib install
	$(INSTALL_DIR) -p $(DESTDIR)$(INST_DIR)
	$(INSTALL_PROG) $(IBIN) $(DESTDIR)$(INST_DIR)

clean:
	$(MAKE) -C lib clean
	rm -rf *.o *~ $(IBIN) $(DEPS) xen TAGS

.PHONY: all clean install libblktap

-include $(DEPS)

