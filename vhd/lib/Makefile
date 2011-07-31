BLKTAP_ROOT := ../../
include $(BLKTAP_ROOT)/Rules.mk

LIBVHD-MAJOR     = 1.0
LIBVHD-MINOR     = 0
LIBVHD-SONAME    = libvhd.so.$(LIBVHD-MAJOR)

LVM-UTIL-OBJ    := $(BLKTAP_ROOT)lvm/lvm-util.o

LIBVHD          := libvhd.a libvhd.so.$(LIBVHD-MAJOR).$(LIBVHD-MINOR)
LIBVHDIO        := libvhdio.a libvhdio.so.$(LIBVHD-MAJOR).$(LIBVHD-MINOR)
LIBVHD-BUILD    := $(LIBVHD) $(LIBVHDIO)

INST-DIR         = /usr/lib

CFLAGS          += -Werror
CFLAGS          += -Wno-unused
CFLAGS          += -I../../include
CFLAGS		+= -I../../lvm
CFLAGS		+= -I../../part
CFLAGS          += -D_GNU_SOURCE
ifndef TRANSFERVM
CFLAGS          += -fPIC
endif
CFLAGS          += -g

LIBS            := -luuid

# Get gcc to generate the dependencies for us.
CFLAGS          += -Wp,-MD,.$(@F).d
DEPS             = .*.d

LIB-SRCS        := libvhd.c
LIB-SRCS        += libvhd-journal.c
LIB-SRCS        += libvhd-index.c
LIB-SRCS        += vhd-util-coalesce.c
LIB-SRCS        += vhd-util-create.c
LIB-SRCS        += vhd-util-fill.c
LIB-SRCS        += vhd-util-modify.c
LIB-SRCS        += vhd-util-query.c
LIB-SRCS        += vhd-util-read.c
LIB-SRCS        += vhd-util-repair.c
LIB-SRCS        += vhd-util-resize.c
LIB-SRCS        += vhd-util-revert.c
LIB-SRCS        += vhd-util-set-field.c
LIB-SRCS        += vhd-util-snapshot.c
LIB-SRCS        += vhd-util-scan.c
LIB-SRCS        += vhd-util-check.c
LIB-SRCS        += relative-path.c
LIB-SRCS        += atomicio.c
LIB-SRCS        += ../../lvm/lvm-util.c

LIB-OBJS         = $(patsubst %.c,%.o,$(LIB-SRCS))

LIBVHD-IO-LIBS  := -ldl -L./ -lvhd
LIBVHD-IO-OBJS  := libvhdio.o $(BLKTAP_ROOT)/part/partition.o
LIBVHD-IO        = libvhdio.a libvhdio.so.$(LIBVHD-MAJOR).$(LIBVHD-MINOR)
LIBVHD-IO-SONAME = libvhdio.so.$(LIBVHD-MAJOR)

all: build

build: $(LIBVHD-BUILD)

libvhd.so: libvhd.so.$(LIBVHD-MAJOR)
	ln -sf $^ $@

libvhdio.so: libvhdio.so.$(LIBVHD-MAJOR)
	ln -sf $^ $@

libvhd.so.$(LIBVHD-MAJOR): libvhd.so.$(LIBVHD-MAJOR).$(LIBVHD-MINOR)
	ln -sf $^ $@

libvhdio.so.$(LIBVHD-MAJOR): libvhdio.so.$(LIBVHD-MAJOR).$(LIBVHD-MINOR)
	ln -sf $^ $@

libvhd.so.$(LIBVHD-MAJOR).$(LIBVHD-MINOR): $(LIB-OBJS)
	$(CC) $(CFLAGS) -Wl,$(SONAME_LDFLAG),$(LIBVHD-SONAME) $(SHLIB_CFLAGS) \
		-o $@ $(LIBS) $^

libvhdio.so.$(LIBVHD-MAJOR).$(LIBVHD-MINOR): $(LIBVHD-IO-OBJS) libvhd.so
	$(CC) $(CFLAGS) -Wl,$(SONAME_LDFLAG),$(LIBVHD-IO-SONAME) $(SHLIB_CFLAGS) \
		-o $@ $(LIBVHD-IO-LIBS) $^

libvhd.a: $(LIB-OBJS)
	$(AR) rc $@ $^

libvhdio.a: $(LIBVHD-IO-OBJS)
	$(AR) rc $@ $^

install: all
	$(INSTALL_DIR) -p $(DESTDIR)$(INST-DIR)
	$(INSTALL_DATA) $(LIBVHD) $(LIBVHD-IO) $(DESTDIR)$(INST-DIR)
	ln -sf libvhd.so.$(LIBVHD-MAJOR).$(LIBVHD-MINOR) $(DESTDIR)$(INST-DIR)/libvhd.so.$(LIBVHD-MAJOR)
	ln -sf libvhd.so.$(LIBVHD-MAJOR) $(DESTDIR)$(INST-DIR)/libvhd.so
	ln -sf libvhdio.so.$(LIBVHD-MAJOR).$(LIBVHD-MINOR) $(DESTDIR)$(INST-DIR)/libvhdio.so.$(LIBVHD-MAJOR)
	ln -sf libvhdio.so.$(LIBVHD-MAJOR) $(DESTDIR)$(INST-DIR)/libvhdio.so

clean:
	rm -rf *.a *.so* *.o *~ $(DEPS) $(LIBVHD) $(LIBVHD-IO)

.PHONY: all build clean install libvhd

-include $(DEPS)
