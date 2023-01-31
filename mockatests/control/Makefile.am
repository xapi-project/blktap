CFLAGS:=$(filter-out -fsanitize=leak,$(LDFLAGS))
LDFLAGS:=$(filter-out -static-liblsan,$(LDFLAGS))

AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror
AM_CFLAGS += -fprofile-dir=/tmp/coverage/blktap/mockatests/control -fprofile-arcs -ftest-coverage
AM_CFLAGS += -Og -fno-inline -g

AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/control -I../include

check_PROGRAMS = test-control
TESTS = test-control

test_control_SOURCES = test-control.c test-tap-ctl-free.c test-tap-ctl-allocate.c test-tap-ctl-close.c test-tap-ctl-list.c control-wrappers.c util.c
test_control_LDFLAGS = $(top_srcdir)/control/libblktapctl.la -lcmocka -luuid

test_control_LDFLAGS += -static-libtool-libs
test_control_LDFLAGS += ../wrappers/libwrappers.la
# Would be good to use the cmocka malloc wraps but looks like maybe strdup doesn't call malloc
#test_control_LDFLAGS += -Wl,--wrap=malloc,--wrap=free
test_control_LDFLAGS += -Wl,--wrap=socket,--wrap=connect,--wrap=read,--wrap=select,--wrap=write,--wrap=fdopen
test_control_LDFLAGS += -Wl,--wrap=open,--wrap=ioctl,--wrap=close,--wrap=access,--wrap=mkdir,--wrap=flock,--wrap=unlink,--wrap=__xmknod
#test_control_LDFLAGS += -Wl,--wrap=execl,--wrap=waitpid
test_control_LDFLAGS += -Wl,--wrap=glob,--wrap=globfree
test_control_LDFLAGS += -Wl,--wrap=fopen,--wrap=fclose,--wrap=fseek,--wrap=fwrite

clean-local:
	-rm -rf *.gc??
