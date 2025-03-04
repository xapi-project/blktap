
SUBDIRS = lib

AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror
AM_CFLAGS += $(if $(GCOV),-fprofile-arcs -ftest-coverage)

AM_CPPFLAGS  = -D_GNU_SOURCE
AM_CPPFLAGS += -I$(top_srcdir)/include

bin_PROGRAMS  = vhd-util
bin_PROGRAMS += vhd-index

LDADD = lib/libvhd.la -luuid

vhd_index_LDADD = lib/libvhd.la -luuid

clean-local:
	-rm -rf *.gc??
