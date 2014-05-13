AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror
AM_CFLAGS += -Wextra

AM_CPPFLAGS  = -D_GNU_SOURCE # required by vasprintf
AM_CPPFLAGS += -I$(top_srcdir)/include
AM_CPPFLAGS += -I$(top_srcdir)/control

bin_PROGRAMS = tapback

tapback_SOURCES  = xenstore.c
tapback_SOURCES += frontend.c
tapback_SOURCES += backend.c
tapback_SOURCES += tapback.h
tapback_SOURCES += tapback.c
tapback_SOURCES += init.d-tapback

tapback_LDADD  = -lxenstore
tapback_LDADD += $(top_srcdir)/control/libblktapctl.la

# Have "exec" in name to ensure it's done trough the install-exec route
# (and before install-exec-local)
exec_init_ddir = $(sysconfdir)/rc.d/init.d
dist_exec_init_d_SCRIPTS = init.d-tapback

logrotatedir = $(sysconfdir)/logrotate.d
dist_logrotate_DATA = tapback-logrotate

install-exec-local:
	cd ${DESTDIR}${exec_init_ddir} && mv init.d-tapback tapback
