
AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror

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
