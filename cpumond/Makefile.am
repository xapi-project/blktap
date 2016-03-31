
AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror

bin_PROGRAMS = cpumond

cpumond_SOURCES  = cpumond.c
cpumond_SOURCES += cpumond.h

cpumond_LDADD  = -lrt

# Have "exec" in name to ensure it's done trough the install-exec route
# (and before install-exec-local)
exec_init_ddir = /usr/lib/systemd/system
dist_exec_init_d_SCRIPTS = cpumond.service
