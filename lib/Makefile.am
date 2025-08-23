AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror
AM_CFLAGS += $(if $(GCOV),-fprofile-dir=/tmp/coverage/blktap/lib -fprofile-arcs -ftest-coverage)

AM_CPPFLAGS = -I$(top_srcdir)/include

lib_LTLIBRARIES = libblktaputil.la

libblktaputil_la_SOURCES = util.c
libblktaputil_la_LDFLAGS = -version-info 1:0:0
libblktaputil_la_LIBADD = -lssl -lcrypto

clean-local:
	-rm -rf *.gc??
