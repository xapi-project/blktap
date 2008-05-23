BLKTAP_ROOT= ..
include $(BLKTAP_ROOT)/Rules.mk

IBIN            = tapdisk #td-util
LOCK_UTIL       = lock-util
VHD_UPDATE_UTIL = vhd-update
INST_DIR        = /usr/sbin

CFLAGS   += -Werror -g
CFLAGS   += -Wno-unused
CFLAGS   += -fno-strict-aliasing
CFLAGS   += -I../include
CFLAGS   += -D_GNU_SOURCE
CFLAGS   += -DUSE_NFS_LOCKS

# Get gcc to generate the dependencies for us.
CFLAGS   += -Wp,-MD,.$(@F).d
DEPS      = .*.d

#tapdisk td-util vhd-update: LIBS += -luuid
tapdisk: LIBS += -luuid

ifneq ($(USE_SYSTEM_LIBRARIES),y)
LIBAIO_DIR = $(XEN_ROOT)/tools/libaio/src
#tapdisk td-util vhd-update: AIOLIBS := $(LIBAIO_DIR)/libaio.a
#tapdisk td-util vhd-update: CFLAGS  += -I$(LIBAIO_DIR) -I$(XEN_LIBXC)
tapdisk: AIOLIBS := $(LIBAIO_DIR)/libaio.a
tapdisk: CFLAGS  += -I$(LIBAIO_DIR) -I$(XEN_LIBXC)
else
#tapdisk td-util vhd-update: AIOLIBS := -laio
tapdisk: AIOLIBS := -laio
endif

TAP-OBJS  := scheduler.o
TAP-OBJS  += tapdisk-ipc.o
TAP-OBJS  += tapdisk-vbd.o
TAP-OBJS  += tapdisk-image.o
TAP-OBJS  += tapdisk-driver.o
TAP-OBJS  += tapdisk-interface.o

BLK-OBJS  := block-aio.o
BLK-OBJS  += block-ram.o
BLK-OBJS  += block-vhd.o
BLK-OBJS  += lock.o
BLK-OBJS  += atomicio.o
BLK-OBJS  += io-optimize.o
#BLK-OBJS  += vhd-coalesce.o
#BLK-OBJS  += vhd-fill.o
#BLK-OBJS  += vhd-read.o
BLK-OBJS  += tapdisk-queue.o
BLK-OBJS  += tapdisk-filter.o
BLK-OBJS  += relative-path.o
BLK-OBJS  += tapdisk-log.o
BLK-OBJS  += tapdisk-utils.o

all: $(IBIN) lock-util #$(VHD_UPDATE_UTIL)

tapdisk: $(TAP-OBJS) $(BLK-OBJS) tapdisk-server.c
	$(CC) $(CFLAGS) -o tapdisk $(TAP-OBJS) $(BLK-OBJS) tapdisk-server.c \
		$(AIOLIBS) $(LIBS)

td-util: $(BLK-OBJS) td.o
	$(CC) $(CFLAGS) -o td-util $(BLK-OBJS) td.o $(AIOLIBS) $(LIBS)

lock-util: lock.c
	$(CC) $(CFLAGS) -DUTIL -o lock-util lock.c $(LIBS)

vhd-update: $(BLK-OBJS) vhd-update.o
	$(CC) $(CFLAGS) -o vhd-update $(BLK-OBJS) vhd-update.o $(LIBS) $(AIOLIBS)

install: all
	$(INSTALL_DIR) -p $(DESTDIR)$(INST_DIR)
#	$(INSTALL_PROG) $(IBIN) $(LOCK_UTIL) $(VHD_UPDATE_UTIL) $(DESTDIR)$(INST_DIR)
	$(INSTALL_PROG) $(IBIN) $(LOCK_UTIL) $(DESTDIR)$(INST_DIR)

clean:
	rm -rf *.o *~ $(DEPS) xen TAGS $(IBIN) $(LIB) $(LOCK_UTIL) # $(VHD_UPDATE_UTIL)

.PHONY: clean install

-include $(DEPS)
