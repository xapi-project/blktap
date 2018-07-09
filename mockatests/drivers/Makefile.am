AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror
AM_CFLAGS += -fprofile-dir=/tmp/coverage/blktap/mockatests/drivers -fprofile-arcs -ftest-coverage
AM_CFLAGS += -Og -fno-inline-functions -g

AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/drivers -I../include

check_PROGRAMS = test-drivers
TESTS = test-drivers

test_drivers_SOURCES = test-drivers.c test-tapdisk-stats.c
test_drivers_LDFLAGS = $(top_srcdir)/drivers/libtapdisk.la -lcmocka -luuid
