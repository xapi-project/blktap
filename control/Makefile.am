
AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror

AM_CPPFLAGS  = -D_GNU_SOURCE
AM_CPPFLAGS += -DTAPCTL
AM_CPPFLAGS += -I$(top_srcdir)/include
AM_CPPFLAGS += -DTAPDISK_EXEC='"tapdisk"'
AM_CPPFLAGS += -DTAPDISK_EXECDIR='"$(libexecdir)"'

sbin_PROGRAMS = tap-ctl
tap_ctl_LDADD = libblktapctl.la

lib_LTLIBRARIES = libblktapctl.la

libblktapctl_la_SOURCES  = tap-ctl-ipc.c
libblktapctl_la_SOURCES += tap-ctl-list.c
libblktapctl_la_SOURCES += tap-ctl-allocate.c
libblktapctl_la_SOURCES += tap-ctl-free.c
libblktapctl_la_SOURCES += tap-ctl-create.c
libblktapctl_la_SOURCES += tap-ctl-destroy.c
libblktapctl_la_SOURCES += tap-ctl-spawn.c
libblktapctl_la_SOURCES += tap-ctl-attach.c
libblktapctl_la_SOURCES += tap-ctl-detach.c
libblktapctl_la_SOURCES += tap-ctl-open.c
libblktapctl_la_SOURCES += tap-ctl-close.c
libblktapctl_la_SOURCES += tap-ctl-pause.c
libblktapctl_la_SOURCES += tap-ctl-unpause.c
libblktapctl_la_SOURCES += tap-ctl-major.c
libblktapctl_la_SOURCES += tap-ctl-check.c
libblktapctl_la_SOURCES += tap-ctl-stats.c

libblktapctl_la_LDFLAGS = -release $(VERSION)

udev_rulesdir = $(sysconfdir)/udev/rules.d
dist_udev_rules_DATA = blktap.rules
