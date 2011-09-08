
SUBDIRS = . $(MAYBE_test)

AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror

AM_CPPFLAGS  = -D_GNU_SOURCE
AM_CPPFLAGS += -I$(top_srcdir)/include
AM_CPPFLAGS += -I$(top_srcdir)/lvm
AM_CPPFLAGS += -I$(top_srcdir)/part

lib_LTLIBRARIES  = libvhd.la
lib_LTLIBRARIES += $(MAYBE_libvhdio_la)

libvhd_la_SOURCES  = libvhd.c
libvhd_la_SOURCES += libvhd-journal.c
libvhd_la_SOURCES += libvhd-index.c
libvhd_la_SOURCES += vhd-util-coalesce.c
libvhd_la_SOURCES += vhd-util-create.c
libvhd_la_SOURCES += vhd-util-fill.c
libvhd_la_SOURCES += vhd-util-modify.c
libvhd_la_SOURCES += vhd-util-query.c
libvhd_la_SOURCES += vhd-util-read.c
libvhd_la_SOURCES += vhd-util-repair.c
libvhd_la_SOURCES += vhd-util-resize.c
libvhd_la_SOURCES += vhd-util-revert.c
libvhd_la_SOURCES += vhd-util-set-field.c
libvhd_la_SOURCES += vhd-util-snapshot.c
libvhd_la_SOURCES += vhd-util-scan.c
libvhd_la_SOURCES += vhd-util-check.c
libvhd_la_SOURCES += relative-path.c
libvhd_la_SOURCES += relative-path.h
libvhd_la_SOURCES += atomicio.c
libvhd_la_SOURCES += atomicio.h
libvhd_la_SOURCES += ../../lvm/lvm-util.c

libvhd_la_LDFLAGS = -version-info 1:1:1

libvhd_la_LIBADD = -luuid $(LIBICONV)

libvhdio_la_SOURCES  = libvhdio.c
libvhdio_la_SOURCES += ../../part/partition.c

libvhdio_la_LDFLAGS  = -release $(VERSION)
libvhdio_la_LDFLAGS += -shared

libvhdio_la_LIBADD  = libvhd.la
libvhdio_la_LIBADD += -ldl

if ENABLE_VHDIO
MAYBE_libvhdio_la = libvhdio.la
endif

if ENABLE_TESTS
MAYBE_test = test
endif
