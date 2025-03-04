
AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror
AM_CFLAGS += $(if $(GCOV),-fprofile-arcs -ftest-coverage)

AM_CPPFLAGS  = -D_GNU_SOURCE
AM_CPPFLAGS += -I$(top_srcdir)/include

bin_PROGRAMS = cpumond

cpumond_SOURCES  = cpumond.c
cpumond_SOURCES += cpumond.h
cpumond_SOURCES += cpumond.service

cpumond_LDADD  = -lrt

SYSTEMD_SERVICE_DIR = /usr/lib/systemd/system

install-exec-local:
	mkdir -p $(DESTDIR)$(SYSTEMD_SERVICE_DIR)
	install -m 644 cpumond.service \
	  $(DESTDIR)$(SYSTEMD_SERVICE_DIR)

clean-local:
	-rm -rf *.gc??
