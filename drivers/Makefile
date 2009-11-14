BLKTAP_ROOT= ..
include $(BLKTAP_ROOT)/Rules.mk

LIBVHDDIR  = $(BLKTAP_ROOT)/vhd/lib

IBIN       = tapdisk tapdisk2 td-util tapdisk-client tapdisk-stream tapdisk-diff
LOCK_UTIL  = lock-util
INST_DIR   = /usr/sbin

CFLAGS    += -Werror -g
CFLAGS    += -Wno-unused
CFLAGS    += -fno-strict-aliasing
CFLAGS    += -I../include
CFLAGS    += -D_GNU_SOURCE
CFLAGS    += -DUSE_NFS_LOCKS

LIBS      += -lrt -lm

# Get gcc to generate the dependencies for us.
CFLAGS    += -Wp,-MD,.$(@F).d
DEPS       = .*.d

tapdisk tapdisk2 td-util tapdisk-stream tapdisk-diff: LIBS += -L$(LIBVHDDIR) -lvhd -luuid

ifneq ($(USE_SYSTEM_LIBRARIES),y)
LIBAIO_DIR = $(XEN_ROOT)/tools/libaio/src
tapdisk tapdisk2 tapdisk-stream tapdisk-diff: AIOLIBS := $(LIBAIO_DIR)/libaio.a
tapdisk tapdisk-client tapdisk-stream tapdisk-diff: CFLAGS  += -I$(LIBAIO_DIR) -I$(XEN_LIBXC)
else
tapdisk tapdisk2 tapdisk-stream tapdisk-diff: AIOLIBS := -laio
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
TAP-OBJS  += tapdisk-server.o
TAP-OBJS  += tapdisk-queue.o
TAP-OBJS  += tapdisk-filter.o
TAP-OBJS  += tapdisk-log.o
TAP-OBJS  += tapdisk-event-log.o
TAP-OBJS  += tapdisk-utils.o
TAP-OBJS  += io-optimize.o
TAP-OBJS  += lock.o

MISC-OBJS := atomicio.o

BLK-OBJS  := block-aio.o
BLK-OBJS  += block-ram.o
BLK-OBJS  += block-cache.o
BLK-OBJS  += block-vhd.o
BLK-OBJS  += block-vindex.o
BLK-OBJS  += block-log.o

all: $(IBIN) lock-util

tapdisk: $(TAP-OBJS) $(BLK-OBJS) $(MISC-OBJS) tapdisk.c
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS) $(AIOLIBS)

tapdisk2: $(TAP-OBJS) $(BLK-OBJS) $(MISC-OBJS) tapdisk2.c
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS) $(AIOLIBS)

tapdisk-client: tapdisk-client.o
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

tapdisk-stream tapdisk-diff: %: %.o $(TAP-OBJS) $(BLK-OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS) $(AIOLIBS)

td-util: td.o tapdisk-utils.o
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

lock-util: lock.c
	$(CC) $(CFLAGS) -DUTIL -o lock-util lock.c $(LIBS)

install: all
	$(INSTALL_DIR) -p $(DESTDIR)$(INST_DIR)
	$(INSTALL_PROG) $(IBIN) $(LOCK_UTIL) $(DESTDIR)$(INST_DIR)

clean:
	rm -rf *.o *~ $(DEPS) xen TAGS $(IBIN) $(LIB) $(LOCK_UTIL)

.PHONY: clean install

-include $(DEPS)
