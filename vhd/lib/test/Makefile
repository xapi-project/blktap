BLKTAP_ROOT := ../../../
include $(BLKTAP_ROOT)/Rules.mk

CFLAGS            += -Werror
CFLAGS            += -Wno-unused
CFLAGS            += -I../include
CFLAGS            += -D_GNU_SOURCE

# Get gcc to generate the dependencies for us.
CFLAGS            += -Wp,-MD,.$(@F).d
DEPS               = .*.d

BINS              := random-copy test-snapshot

all: $(BINS)

%: %.o
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -rf *.o *~ $(DEPS) $(BINS)

.PHONY: all clean

-include $(DEPS)
