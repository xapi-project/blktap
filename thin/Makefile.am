
AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror
AM_CFLAGS += -pthread

AM_CPPFLAGS  = -D_GNU_SOURCE
AM_CPPFLAGS += -I$(top_srcdir)/include

sbin_PROGRAMS = thin-tapdisk
thin_tapdisk_SOURCES = sock_serv.c
thin_tapdisk_LDADD = libtapdiskthin.la

sbin_PROGRAMS += thin-cli
thin_cli_SOURCES = thin_cli.c
thin_cli_LDADD = libtapdiskthin.la

lib_LTLIBRARIES = libtapdiskthin.la

libtapdiskthin_la_SOURCES = payload.c
libtapdiskthin_la_SOURCES += sock_client2.c
libtapdiskthin_la_SOURCES += kpr_util.c

libtapdiskthin_la_LDFLAGS = -version-info 1:1:1

