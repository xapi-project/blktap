XEN_ROOT = ../../..
include $(XEN_ROOT)/tools/Rules.mk

IBIN         = blktapctrl tapdisk td-util
LOCK_UTIL    = lock-util
INST_DIR     = /usr/sbin

CFLAGS   += -Werror -g
CFLAGS   += -Wno-unused
CFLAGS   += -fno-strict-aliasing
CFLAGS   += -I../lib
CFLAGS   += -D_GNU_SOURCE
CFLAGS   += -DUSE_NFS_LOCKS

# Get gcc to generate the dependencies for us.
CFLAGS   += -Wp,-MD,.$(@F).d
DEPS      = .*.d

blktapctrl:      LIBS += -L../lib
blktapctrl:      LIBS += -lblktap -lxenctrl
blktapctrl:      LIBS += -lxenstore
tapdisk td-util: LIBS += -luuid

ifneq ($(USE_SYSTEM_LIBRARIES),y)
LIBAIO_DIR = $(XEN_ROOT)/tools/libaio/src
tapdisk td-util: AIOLIBS := $(LIBAIO_DIR)/libaio.a
blktapctrl:      CFLAGS  += -I$(LIBAIO_DIR) -I$(XEN_LIBXC) -I$(XEN_ROOT)/tools/xenstore
blktapctrl:      LIBS    += -L$(XEN_LIBXC) -L$(XEN_XENSTORE)
else
tapdisk td-util: AIOLIBS := -laio
endif

BLK-OBJS  := block-aio.o
BLK-OBJS  += block-vhd.o
BLK-OBJS  += block-ram.o
BLK-OBJS  += lock.o
BLK-OBJS  += atomicio.o
BLK-OBJS  += io-optimize.o
BLK-OBJS  += vhd-coalesce.o
BLK-OBJS  += vhd-fill.o

all: $(IBIN) lock-util

LINUX_ROOT := $(wildcard $(XEN_ROOT)/linux-2.6.*-xen-sparse)

blktapctrl: blktapctrl.c
	$(CC) $(CFLAGS) -o blktapctrl $(LIBS) blktapctrl.c

tapdisk: $(BLK-OBJS) tapdisk.c
	$(CC) $(CFLAGS) -o tapdisk $(BLK-OBJS) tapdisk.c \
		$(AIOLIBS) $(LIBS)

td-util: $(BLK-OBJS) td.o
	$(CC) $(CFLAGS) -o td-util $(BLK-OBJS) td.o $(AIOLIBS) $(LIBS)

lock-util: lock.c
	$(CC) $(CFLAGS) -DUTIL -o lock-util lock.c $(LIBS)


install: all
	$(INSTALL_DIR) -p $(DESTDIR)$(INST_DIR)
	$(INSTALL_PROG) $(IBIN) $(LOCK_UTIL) $(DESTDIR)$(INST_DIR)

clean:
	rm -rf *.o *~ $(DEPS) xen TAGS $(IBIN) $(LIB) $(LOCK_UTIL) 

.PHONY: clean install

-include $(DEPS)
