
AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror

bin_PROGRAMS = cumd

cumd_SOURCES  = cumd.c
cumd_SOURCES += cumd.h

cumd_LDADD  = -lrt

# Have "exec" in name to ensure it's done trough the install-exec route
# (and before install-exec-local)
exec_init_ddir = /usr/lib/systemd/system
dist_exec_init_d_SCRIPTS = cumd.service
