AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror
AM_CFLAGS += -Wextra

AM_CPPFLAGS  = -D_GNU_SOURCE # required by vasprintf
AM_CPPFLAGS += -I$(top_srcdir)/include
AM_CPPFLAGS += -I$(top_srcdir)/control

bin_PROGRAMS = tapback

tapback_SOURCES  = log.c
tapback_SOURCES += xenstore.c
tapback_SOURCES += frontend.c
tapback_SOURCES += backend.c
tapback_SOURCES += tapback.h
tapback_SOURCES += tapback.c

tapback_LDADD  = -lxenstore
tapback_LDADD += $(top_srcdir)/control/libblktapctl.la
