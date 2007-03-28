XEN_ROOT = ../../..
include $(XEN_ROOT)/tools/Rules.mk

IBIN         = blktapctrl tapdisk
QCOW_UTIL    = img2qcow qcow2raw qcow-create qcow-query
VHD_UTIL     = vhd-create
INST_DIR     = /usr/sbin

CFLAGS   += -Werror
CFLAGS   += -Wno-unused
CFLAGS   += -fno-strict-aliasing
CFLAGS   += -I../lib
CFLAGS   += -D_GNU_SOURCE
CFLAGS   += -DUSE_NFS_LOCKS
#CFLAGS   += -DEXCLUSIVE_LOCK

# Get gcc to generate the dependencies for us.
CFLAGS   += -Wp,-MD,.$(@F).d
DEPS      = .*.d

THREADLIB := -lpthread -lz
LIBS      := -L../lib
LIBS      += -lblktap -lxenctrl
LIBS      += -lcrypto
LIBS      += -lz
LIBS      += -luuid
LIBS      += -lxenstore

ifneq ($(USE_SYSTEM_LIBRARIES),y)
LIBAIO_DIR = $(XEN_ROOT)/tools/libaio/src
AIOLIBS   := $(LIBAIO_DIR)/libaio.a
CFLAGS    += -I$(LIBAIO_DIR) -I$(XEN_LIBXC) -I$(XEN_ROOT)/tools/xenstore
LIBS      += -L$(XEN_LIBXC) -L$(XEN_XENSTORE)
else
AIOLIBS   := -laio
endif

BLK-OBJS  := block-aio.o
BLK-OBJS  += block-sync.o
BLK-OBJS  += block-vmdk.o
BLK-OBJS  += block-vhdsync.o
BLK-OBJS  += block-vhd.o
BLK-OBJS  += block-ram.o
BLK-OBJS  += block-qcow.o
BLK-OBJS  += aes.o
BLK-OBJS  += lockex.o
BLK-OBJS  += lock.o
BLK-OBJS  += atomicio.o

all: $(IBIN) vhd-util qcow-util

LINUX_ROOT := $(wildcard $(XEN_ROOT)/linux-2.6.*-xen-sparse)


blktapctrl: blktapctrl.c
	$(CC) $(CFLAGS) -o blktapctrl $(LIBS) blktapctrl.c

tapdisk: $(BLK-OBJS) tapdisk.c
	$(CC) $(CFLAGS) -o tapdisk $(BLK-OBJS) tapdisk.c \
		$(AIOLIBS) $(LIBS)

.PHONY: qcow-util
qcow-util: $(QCOW_UTIL)

$(QCOW_UTIL): %: $(BLK-OBJS)
	$(CC) $(CFLAGS) -o $* $(BLK-OBJS) $*.c $(AIOLIBS) $(LIBS)

.PHONY: vhd-util
vhd-util: $(VHD_UTIL)

$(VHD_UTIL): %: $(BLK-OBJS)
	$(CC) $(CFLAGS) -o $* $(BLK-OBJS) $*.c $(AIOLIBS) $(LIBS)


install: all
	$(INSTALL_PROG) $(IBIN) $(QCOW_UTIL) $(VHD_UTIL) $(DESTDIR)$(INST_DIR)

clean:
	rm -rf *.o *~ $(DEPS) xen TAGS $(IBIN) $(LIB) $(QCOW_UTIL) $(VHD_UTIL)

.PHONY: clean install

-include $(DEPS)
