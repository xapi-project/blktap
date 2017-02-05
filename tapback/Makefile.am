AM_CFLAGS  = -Wall
AM_CFLAGS += -Wwrite-strings
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
tapback_SOURCES += tapback.service

tapback_LDADD  = -lxenstore
tapback_LDADD += $(top_srcdir)/control/libblktapctl.la


# Have "exec" in name to ensure it's done trough the install-exec route
# (and before install-exec-local)
SYSTEMD_SERVICE_DIR = /usr/lib/systemd/system

install-exec-local:
	mkdir -p $(DESTDIR)$(SYSTEMD_SERVICE_DIR)
	install -m 644 tapback.service \
	  $(DESTDIR)$(SYSTEMD_SERVICE_DIR)
