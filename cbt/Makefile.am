AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror
# Need this to be conditionalised
#AM_CFLAGS += -fprofile-arcs
#AM_CFLAGS += -ftest-coverage

AM_CPPFLAGS = -I$(top_srcdir)/include

sbin_PROGRAMS = cbt-util

noinst_LTLIBRARIES = libcbtutil.la

libcbtutil_la_SOURCES = cbt-util.c

cbt_util_SOURCES  = main.c
cbt_util_LDADD  = -lrt -luuid libcbtutil.la
