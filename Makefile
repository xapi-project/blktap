BLKTAP_ROOT := .
include $(BLKTAP_ROOT)/Rules.mk

SUBDIRS-y :=
SUBDIRS-y += include
SUBDIRS-y += lvm
SUBDIRS-y += part
SUBDIRS-y += vhd
SUBDIRS-y += drivers
SUBDIRS-y += control

.PHONY: all
all: build

.PHONY: build
build:
	@set -e; for subdir in $(SUBDIRS-y); do \
	$(MAKE) -C $$subdir all;       \
		done

.PHONY: install
install:
	@set -e; for subdir in $(SUBDIRS-y); do \
		$(MAKE) -C $$subdir install; \
	done

.PHONY: clean
clean:
	rm -rf *.a *.so *.o *.rpm $(LIB) *~ $(DEPS) TAGS
	@set -e; for subdir in $(SUBDIRS-y); do \
	$(MAKE) -C $$subdir clean;       \
		done
