## export XEN_ROOT = /path/to/xen/tree
## if XEN_ROOT is not otherwise set, assume standard xen tree structure
XEN_ROOT = ../..
include $(XEN_ROOT)/tools/Rules.mk

SUBDIRS-y :=
SUBDIRS-y += include
SUBDIRS-y += drivers
SUBDIRS-y += daemon

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
