
# ACLOCAL_AMFLAGS = -I m4

# FIXME run cppcheck

SUBDIRS  = lvm
SUBDIRS += $(MAYBE_part)
SUBDIRS += vhd
SUBDIRS += thin
SUBDIRS += control
SUBDIRS += drivers
SUBDIRS += include
SUBDIRS += tapback

if ENABLE_PART
MAYBE_part = part
endif
