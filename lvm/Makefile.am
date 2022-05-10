
AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror
AM_CFLAGS += $(if $(GCOV),-fprofile-dir=/tmp/coverage/blktap/lvm -fprofile-arcs -ftest-coverage)

sbin_PROGRAMS = lvm-util

noinst_LTLIBRARIES = liblvmutil.la

liblvmutil_la_SOURCES  = lvm-util.c

lvm_util_SOURCES = main.c
lvm_util_LDADD = liblvmutil.la

liblvmutil_la_CPPFLAGS  = -D_GNU_SOURCE
liblvmutil_la_CPPFLAGS += -I$(top_srcdir)/include

clean-local:
	-rm -rf *.gc??
