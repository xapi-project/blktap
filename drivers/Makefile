BLKTAP_ROOT= ..
include $(BLKTAP_ROOT)/Rules.mk

LIBVHDDIR  = $(BLKTAP_ROOT)/vhd/lib

IBIN       = tapdisk td-util
LOCK_UTIL  = lock-util
INST_DIR   = /usr/sbin

CFLAGS    += -Werror -g
CFLAGS    += -Wno-unused
CFLAGS    += -fno-strict-aliasing
CFLAGS    += -I../include
CFLAGS    += -D_GNU_SOURCE
CFLAGS    += -DUSE_NFS_LOCKS

LIBS      += -lrt

# Get gcc to generate the dependencies for us.
CFLAGS    += -Wp,-MD,.$(@F).d
DEPS       = .*.d

tapdisk td-util: LIBS += -L$(LIBVHDDIR) -lvhd -luuid

ifneq ($(USE_SYSTEM_LIBRARIES),y)
LIBAIO_DIR = $(XEN_ROOT)/tools/libaio/src
tapdisk: AIOLIBS := $(LIBAIO_DIR)/libaio.a
tapdisk: CFLAGS  += -I$(LIBAIO_DIR) -I$(XEN_LIBXC)
else
tapdisk: AIOLIBS := -laio
endif

ifeq ($(VHD_STATIC),y)
td-util: CFLAGS += -static
endif

TAP-OBJS  := scheduler.o
TAP-OBJS  += tapdisk-ipc.o
TAP-OBJS  += tapdisk-vbd.o
TAP-OBJS  += tapdisk-image.o
TAP-OBJS  += tapdisk-driver.o
TAP-OBJS  += tapdisk-interface.o

MISC-OBJS := atomicio.o

BLK-OBJS  := block-aio.o
BLK-OBJS  += block-ram.o
BLK-OBJS  += block-cache.o
BLK-OBJS  += block-vhd.o
BLK-OBJS  += block-vindex.o
BLK-OBJS  += block-log.o
BLK-OBJS  += lock.o
BLK-OBJS  += io-optimize.o
BLK-OBJS  += tapdisk-queue.o
BLK-OBJS  += tapdisk-filter.o
BLK-OBJS  += tapdisk-log.o
BLK-OBJS  += tapdisk-utils.o

all: $(IBIN) lock-util

tapdisk: $(TAP-OBJS) $(MISC-OBJS) $(BLK-OBJS) tapdisk-server.c
	$(CC) $(CFLAGS) -o tapdisk $(TAP-OBJS) $(MISC-OBJS) $(BLK-OBJS) \
		tapdisk-server.c $(AIOLIBS) $(LIBS)

td-util: td.o tapdisk-utils.o
	$(CC) $(CFLAGS) -o td-util td.o tapdisk-utils.o $(LIBS)

lock-util: lock.c
	$(CC) $(CFLAGS) -DUTIL -o lock-util lock.c $(LIBS)

install: all
	$(INSTALL_DIR) -p $(DESTDIR)$(INST_DIR)
	$(INSTALL_PROG) $(IBIN) $(LOCK_UTIL) $(DESTDIR)$(INST_DIR)

clean:
	rm -rf *.o *~ $(DEPS) xen TAGS $(IBIN) $(LIB) $(LOCK_UTIL)

.PHONY: clean install

-include $(DEPS)
