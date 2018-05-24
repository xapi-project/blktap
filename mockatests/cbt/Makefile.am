AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror
# Arc coverage and wrap don't seem to be compatible
#AM_CFLAGS += -fprofile-arcs
#AM_CFLAGS += -ftest-coverage

AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/cbt

check_PROGRAMS = test-cbt-util
TESTS = test-cbt-util

test_cbt_util_SOURCES = test-cbt-util.c
test_cbt_util_LDFLAGS = $(top_srcdir)/cbt/libcbtutil.la -lcmocka -luuid
test_cbt_util_LDFLAGS += -Wl,--wrap=fopen,--wrap=fclose,--wrap=printf
