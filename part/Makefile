BLKTAP_ROOT := ../
include $(BLKTAP_ROOT)/Rules.mk

IBIN               = part-util
INST_DIR           = /usr/sbin

VHDPARTX           = vhdpartx

CFLAGS            += -g -O2
CFLAGS            += -Werror
CFLAGS            += -Wno-unused
CFLAGS            += -I../include
CFLAGS            += -D_GNU_SOURCE

# Get gcc to generate the dependencies for us.
CFLAGS            += -Wp,-MD,.$(@F).d
DEPS               = .*.d

all: build

build: $(IBIN)

part-util: part-util.o partition.o
	$(CC) $(CFLAGS) -o $@ $^

install: all
	$(INSTALL_DIR) -p $(DESTDIR)$(INST_DIR)
	$(INSTALL_PROG) $(IBIN) $(VHDPARTX) $(DESTDIR)$(INST_DIR)

clean:
	rm -rf *.o *~ $(DEPS) $(IBIN)

.PHONY: all build clean install

-include $(DEPS)
