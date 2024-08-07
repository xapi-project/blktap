AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror
AM_CFLAGS += -fprofile-dir=/tmp/coverage/blktap/mockatests/vhd -fprofile-arcs -ftest-coverage
AM_CFLAGS += -Og -fno-inline-functions -g
AM_CFLAGS += -Doff64_t=__off64_t

AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/cbt -I../include

check_PROGRAMS = test-vhd-util
TESTS = test-vhd-util

test_vhd_util_LDADD = $(top_srcdir)/vhd/lib/libvhd.la

test_vhd_util_SOURCES = test-vhd-util.c test-vhd-util-snapshot.c test-canonpath.c test-bitops.c test-vhd-util-utilities.c vhd-wrappers.c
test_vhd_util_LDFLAGS = -lcmocka
test_vhd_util_LDFLAGS += -static-libtool-libs
test_vhd_util_LDFLAGS += -Wl,--wrap=free,--wrap=malloc,--wrap=realloc
test_vhd_util_LDFLAGS += -Wl,--wrap=canonpath
test_vhd_util_LDFLAGS += -Wl,--wrap=vhd_flag_test
test_vhd_util_LDFLAGS += -Wl,--wrap=vhd_snapshot
test_vhd_util_LDFLAGS += -Wl,--wrap=vhd_open
test_vhd_util_LDFLAGS += -Wl,--wrap=vhd_get_keyhash
test_vhd_util_LDFLAGS += -Wl,--wrap=vhd_set_keyhash
test_vhd_util_LDFLAGS += -Wl,--wrap=vhd_close
test_vhd_util_LDFLAGS += -Wl,--wrap=get_current_dir_name,--wrap=realpath
