CFLAGS:=$(filter-out -fsanitize=leak,$(CFLAGS))
LDFLAGS:=$(filter-out -static-liblsan,$(LDFLAGS))

AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror
AM_CFLAGS += -fprofile-dir=/tmp/coverage/blktap/mockatests/cbt -fprofile-arcs -ftest-coverage
AM_CFLAGS += -Og -fno-inline-functions -g

AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/cbt -I../include

check_PROGRAMS = test-cbt-util
TESTS = test-cbt-util

test_cbt_util_LDADD = $(top_srcdir)/cbt/libcbtutil.la ../wrappers/libwrappers.la

test_cbt_util_SOURCES = test-cbt-util.c test-cbt-util-commands.c test-cbt-util-set.c test-cbt-util-get.c test-cbt-util-create.c test-cbt-util-coalesce.c
test_cbt_util_LDFLAGS = -lcmocka
test_cbt_util_LDFLAGS += -Wl,--wrap=fopen,--wrap=fclose
test_cbt_util_LDFLAGS += -Wl,--wrap=malloc,--wrap=free,--wrap=calloc
# Need to wrap both of these as rpmbuild/mock set -D_FORTIFY_SOURCE=2
test_cbt_util_LDFLAGS += -Wl,--wrap=printf,--wrap=__printf_chk
# GCC will "optimise" printf calls - http://www.ciselant.de/projects/gcc_printf/gcc_printf.html
test_cbt_util_LDFLAGS += -Wl,--wrap=puts
test_cbt_util_LDFLAGS += -Wl,--wrap=fwrite
test_cbt_util_LDFLAGS += -Wl,--wrap=fseek

clean-local:
	-rm -rf *.gc??
