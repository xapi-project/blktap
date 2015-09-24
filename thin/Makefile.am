
AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror
AM_CFLAGS += -pthread

AM_CPPFLAGS  = -D_GNU_SOURCE
AM_CPPFLAGS += -I$(top_srcdir)/include

sbin_PROGRAMS = thinprovd
thinprovd_SOURCES = thinprovd.c
thinprovd_LDADD = libtapdiskthin.la

sbin_PROGRAMS += thin-cli
thin_cli_SOURCES = thin_cli.c
thin_cli_LDADD = libtapdiskthin.la

dist_sbin_SCRIPTS = xlvhd-resize
dist_sbin_SCRIPTS += xlvhd-refresh

lib_LTLIBRARIES = libtapdiskthin.la

libtapdiskthin_la_SOURCES = payload.c
libtapdiskthin_la_SOURCES += thin_client.c
libtapdiskthin_la_SOURCES += kpr_util.c
libtapdiskthin_la_SOURCES += thin_log.c

libtapdiskthin_la_LDFLAGS = -version-info 1:1:1

# Have "exec" in name to ensure it's done trough the install-exec route
# (and before install-exec-local)
exec_init_ddir = $(sysconfdir)/rc.d/init.d
dist_exec_init_d_SCRIPTS = thinprovd.init

install-exec-local:
	cd ${DESTDIR}${exec_init_ddir} && mv thinprovd.init thinprovd

