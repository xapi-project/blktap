AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror
# TODO add -Wextra

AM_CPPFLAGS  = -D_GNU_SOURCE
AM_CPPFLAGS += -I$(top_srcdir)/include

libexec_PROGRAMS = tapdisk

tapdisk_SOURCES = tapdisk2.c
tapdisk_LDADD = libtapdisk.la

noinst_PROGRAMS = tapdisk-stream

tapdisk_stream_LDADD = libtapdisk.la

sbin_PROGRAMS  = td-util
sbin_PROGRAMS += td-rated

td_util_SOURCES = td.c
td_util_LDADD = libtapdisk.la

noinst_LTLIBRARIES = libtapdisk.la

libtapdisk_la_SOURCES  = tapdisk.h
libtapdisk_la_SOURCES += scheduler.c
libtapdisk_la_SOURCES += scheduler.h
libtapdisk_la_SOURCES += tapdisk-control.c
libtapdisk_la_SOURCES += tapdisk-control.h
libtapdisk_la_SOURCES += tapdisk-vbd.c
libtapdisk_la_SOURCES += tapdisk-vbd.h
libtapdisk_la_SOURCES += linux-blktap.h
libtapdisk_la_SOURCES += tapdisk-blktap.c
libtapdisk_la_SOURCES += tapdisk-blktap.h
libtapdisk_la_SOURCES += tapdisk-nbdserver.c
libtapdisk_la_SOURCES += tapdisk-nbdserver.h
libtapdisk_la_SOURCES += tapdisk-nbd.h
libtapdisk_la_SOURCES += tapdisk-image.c
libtapdisk_la_SOURCES += tapdisk-image.h
libtapdisk_la_SOURCES += tapdisk-driver.c
libtapdisk_la_SOURCES += tapdisk-driver.h
libtapdisk_la_SOURCES += tapdisk-disktype.c
libtapdisk_la_SOURCES += tapdisk-disktype.h
libtapdisk_la_SOURCES += tapdisk-interface.c
libtapdisk_la_SOURCES += tapdisk-interface.h
libtapdisk_la_SOURCES += tapdisk-server.c
libtapdisk_la_SOURCES += tapdisk-server.h
libtapdisk_la_SOURCES += tapdisk-queue.c
libtapdisk_la_SOURCES += tapdisk-queue.h
libtapdisk_la_SOURCES += libaio-compat.h
libtapdisk_la_SOURCES += tapdisk-filter.c
libtapdisk_la_SOURCES += tapdisk-filter.h
libtapdisk_la_SOURCES += tapdisk-logfile.c
libtapdisk_la_SOURCES += tapdisk-logfile.h
libtapdisk_la_SOURCES += tapdisk-log.c
libtapdisk_la_SOURCES += tapdisk-log.h
libtapdisk_la_SOURCES += tapdisk-utils.c
libtapdisk_la_SOURCES += tapdisk-utils.h
libtapdisk_la_SOURCES += tapdisk-syslog.c
libtapdisk_la_SOURCES += tapdisk-syslog.h
libtapdisk_la_SOURCES += tapdisk-stats.c
libtapdisk_la_SOURCES += tapdisk-stats.h
libtapdisk_la_SOURCES += tapdisk-metrics.c
libtapdisk_la_SOURCES += tapdisk-metrics.h
libtapdisk_la_SOURCES += tapdisk-storage.c
libtapdisk_la_SOURCES += tapdisk-storage.h
libtapdisk_la_SOURCES += tapdisk-loglimit.c
libtapdisk_la_SOURCES += tapdisk-loglimit.h
libtapdisk_la_SOURCES += io-optimize.c
libtapdisk_la_SOURCES += io-optimize.h
libtapdisk_la_SOURCES += lock.c
libtapdisk_la_SOURCES += lock.h
libtapdisk_la_SOURCES += atomicio.c
libtapdisk_la_SOURCES += atomicio.h
libtapdisk_la_SOURCES += tapdisk-fdreceiver.c
libtapdisk_la_SOURCES += tapdisk-fdreceiver.h
libtapdisk_la_SOURCES += md5.c
libtapdisk_la_SOURCES += md5.h

libtapdisk_la_SOURCES += block-aio.c
libtapdisk_la_SOURCES += block-aio.h
libtapdisk_la_SOURCES += block-ram.c
libtapdisk_la_SOURCES += block-cache.c
libtapdisk_la_SOURCES += block-vhd.c
libtapdisk_la_SOURCES += block-valve.c
libtapdisk_la_SOURCES += block-valve.h
libtapdisk_la_SOURCES += block-vindex.c
libtapdisk_la_SOURCES += block-lcache.c
libtapdisk_la_SOURCES += block-llcache.c
libtapdisk_la_SOURCES += block-nbd.c

# shared ring
libtapdisk_la_SOURCES += td-blkif.c
libtapdisk_la_SOURCES += td-blkif.h
libtapdisk_la_SOURCES += td-req.c
libtapdisk_la_SOURCES += td-req.h
libtapdisk_la_SOURCES += td-ctx.c
libtapdisk_la_SOURCES += td-ctx.h
libtapdisk_la_SOURCES += td-stats.c
libtapdisk_la_SOURCES += td-stats.h

libtapdisk_la_LIBADD  = ../vhd/lib/libvhd.la
libtapdisk_la_LIBADD += -laio
libtapdisk_la_LIBADD += -lxenctrl

logrotatedir = $(sysconfdir)/logrotate.d
dist_logrotate_DATA = blktap

bugtooldir = $(sysconfdir)/xensource/bugtool
dist_bugtool_DATA = tapdisk-logs.xml

bugtooltddir = $(sysconfdir)/xensource/bugtool/tapdisk-logs
dist_bugtooltd_DATA = tapdisk-logs/description.xml
