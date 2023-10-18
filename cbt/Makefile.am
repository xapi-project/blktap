AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror
AM_CFLAGS += $(if $(GCOV),-fprofile-dir=/tmp/coverage/blktap/cbt -fprofile-arcs -ftest-coverage)

AM_CPPFLAGS = -I$(top_srcdir)/include

sbin_PROGRAMS = cbt-util

noinst_LTLIBRARIES = libcbtutil.la

libcbtutil_la_SOURCES = cbt-util.c
libcbtutil_la_LIBADD =  -luuid

cbt_util_SOURCES  = main.c
cbt_util_LDADD  = -lrt libcbtutil.la

clean-local:
	-rm -rf *.gc??
