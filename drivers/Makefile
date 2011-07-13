BLKTAP_ROOT= ..
include $(BLKTAP_ROOT)/Rules.mk

LIBVHDDIR  = $(BLKTAP_ROOT)/vhd/lib

IBIN       = tapdisk2 td-util td-rated tapdisk-stream
LOCK_UTIL  = lock-util
INST_DIR   = /usr/sbin

CFLAGS    += -Werror -g
CFLAGS    += -Wno-unused
CFLAGS    += -fno-strict-aliasing
CFLAGS    += -I../include
CFLAGS    += -D_GNU_SOURCE
CFLAGS    += -DUSE_NFS_LOCKS
CFLAGS    += -DTAPDISK

LIBS      += -lrt

# Get gcc to generate the dependencies for us.
CFLAGS    += -Wp,-MD,.$(@F).d
DEPS       = .*.d

tapdisk2 td-util tapdisk-stream tapdisk-diff: LIBS += -L$(LIBVHDDIR) -lvhd -luuid
tapdisk2 tapdisk-stream tapdisk-diff: AIOLIBS := -laio

ifeq ($(VHD_STATIC),y)
td-util: CFLAGS += -static
endif

TAP-OBJS  := scheduler.o
TAP-OBJS  += tapdisk-control.o
TAP-OBJS  += tapdisk-vbd.o
TAP-OBJS  += tapdisk-blktap.o
TAP-OBJS  += tapdisk-image.o
TAP-OBJS  += tapdisk-driver.o
TAP-OBJS  += tapdisk-disktype.o
TAP-OBJS  += tapdisk-interface.o
TAP-OBJS  += tapdisk-server.o
TAP-OBJS  += tapdisk-queue.o
TAP-OBJS  += tapdisk-filter.o
TAP-OBJS  += tapdisk-logfile.o
TAP-OBJS  += tapdisk-log.o
TAP-OBJS  += tapdisk-utils.o
TAP-OBJS  += tapdisk-syslog.o
TAP-OBJS  += tapdisk-stats.o
TAP-OBJS  += tapdisk-storage.o
TAP-OBJS  += tapdisk-loglimit.o
TAP-OBJS  += io-optimize.o
TAP-OBJS  += lock.o

MISC-OBJS := atomicio.o

BLK-OBJS  := block-aio.o
BLK-OBJS  += block-ram.o
BLK-OBJS  += block-cache.o
BLK-OBJS  += block-vhd.o
BLK-OBJS  += block-valve.o
BLK-OBJS  += block-vindex.o
BLK-OBJS  += block-lcache.o
BLK-OBJS  += block-llcache.o

all: $(IBIN) lock-util

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
