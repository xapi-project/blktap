BLKTAP_ROOT := ../../
include $(BLKTAP_ROOT)/Rules.mk

LIBVHD-MAJOR     = 1.0
LIBVHD-MINOR     = 0
LIBVHD-SONAME    = libvhd.so.$(LIBVHD-MAJOR)

LIBVHD-BUILD    := libvhd.a

INST-DIR         = /usr/lib

CFLAGS          += -Werror
CFLAGS          += -Wno-unused
CFLAGS          += -I../../include
CFLAGS          += -D_GNU_SOURCE
CFLAGS          += -fPIC
CFLAGS          += -g

LIBS            := -luuid

# Get gcc to generate the dependencies for us.
CFLAGS          += -Wp,-MD,.$(@F).d
DEPS             = .*.d

LIB-SRCS        := libvhd.c
LIB-SRCS        += libvhd-journal.c
LIB-SRCS        += vhd-util-resize.c
LIB-SRCS        += relative-path.c
LIB-SRCS        += atomicio.c

LIB-OBJS         = $(patsubst %.c,%.o,$(LIB-SRCS))

LIBVHD           = libvhd.a libvhd.so.$(LIBVHD-MAJOR).$(LIBVHD-MINOR)

all: build

build: $(LIBVHD-BUILD)

libvhd.a: $(LIB-OBJS)
	$(CC) $(CFLAGS) -Wl,$(SONAME_LDFLAG),$(LIBVHD-SONAME) $(SHLIB_CFLAGS) \
		-o libvhd.so.$(LIBVHD-MAJOR).$(LIBVHD-MINOR) $(LIBS) $^
	ln -sf libvhd.so.$(LIBVHD-MAJOR).$(LIBVHD-MINOR) libvhd.so.$(LIBVHD-MAJOR)
	ln -sf libvhd.so.$(LIBVHD-MAJOR) libvhd.so
	$(AR) rc $@ $^

install: all
	$(INSTALL_DIR) -p $(DESTDIR)$(INST-DIR)
	$(INSTALL_DATA) $(LIBVHD) $(DESTDIR)$(INST-DIR)
	ln -sf libvhd.so.$(LIBVHD-MAJOR).$(LIBVHD-MINOR) $(DESTDIR)$(INST-DIR)/libvhd.so.$(LIBVHD-MAJOR)
	ln -sf libvhd.so.$(LIBVHD-MAJOR) $(DESTDIR)$(INST-DIR)/libvhd.so

clean:
	rm -rf *.a *.so* *.o *~ $(DEPS) $(LIBVHD)

.PHONY: all build clean install libvhd

-include $(DEPS)
