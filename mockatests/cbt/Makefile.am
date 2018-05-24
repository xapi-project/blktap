AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror
AM_CFLAGS += -fprofile-arcs
AM_CFLAGS += -ftest-coverage

AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/cbt

check_PROGRAMS = test-cbt-util
TESTS = test-cbt-util

test_cbt_util_SOURCES = test-cbt-util.c wrappers.c test-cbt-util-commands.c test-cbt-util-get.c
test_cbt_util_LDFLAGS = $(top_srcdir)/cbt/libcbtutil.la -lcmocka -luuid
test_cbt_util_LDFLAGS += -Wl,--wrap=fopen,--wrap=fclose
# Need to wrap both of these as rpmbuild/mock set -D_FORTIFY_SOURCE=2
test_cbt_util_LDFLAGS += -Wl,--wrap=printf,--wrap=__printf_chk
