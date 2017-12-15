
SUBDIRS = lib

AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror

AM_CPPFLAGS  = -D_GNU_SOURCE
AM_CPPFLAGS += -I$(top_srcdir)/include

bin_PROGRAMS  = vhd-util
bin_PROGRAMS += vhd-index
bin_PROGRAMS += vhd-update

LDADD = lib/libvhd.la -luuid -lcrypto

vhd_index_LDADD = lib/libvhd.la -luuid -lcrypto
