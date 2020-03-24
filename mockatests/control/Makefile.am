AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror
AM_CFLAGS += -fprofile-dir=/tmp/coverage/blktap/mockatests/control -fprofile-arcs -ftest-coverage
AM_CFLAGS += -Og -fno-inline-functions -g

AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/control -I../include

check_PROGRAMS = test-control
TESTS = test-control

test_control_SOURCES = test-control.c test-tap-ctl-free.c control-wrappers.c
test_control_LDFLAGS = $(top_srcdir)/control/.libs/libblktapctl.a -lcmocka -luuid

test_control_LDFLAGS += ../wrappers/libwrappers.la
test_control_LDFLAGS += -Wl,--wrap=connect,--wrap=read,--wrap=select,--wrap=write,--wrap=fdopen
test_control_LDFLAGS += -Wl,--wrap=open,--wrap=ioctl,--wrap=close
test_control_LDFLAGS += -Wl,--wrap=execl,--wrap=waitpid
test_control_LDFLAGS += -Wl,--wrap=fopen,--wrap=fclose,--wrap=fseek,--wrap=fwrite
