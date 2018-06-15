
# ACLOCAL_AMFLAGS = -I m4

# FIXME run cppcheck

SUBDIRS  = lvm
SUBDIRS += $(MAYBE_part)
SUBDIRS += vhd
SUBDIRS += cpumond
SUBDIRS += control
SUBDIRS += drivers
SUBDIRS += include
SUBDIRS += tapback
SUBDIRS += cbt
SUBDIRS += mockatests

if ENABLE_PART
MAYBE_part = part
endif

dist_doc_DATA = README README.md LICENSE CONTRIB MAINTAINERS
