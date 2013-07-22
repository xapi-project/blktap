
AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror
AM_CFLAGS += -Wextra

AM_CPPFLAGS  = -D_GNU_SOURCE # required by vasprintf
#AM_CPPFLAGS += -DTAPCTL
AM_CPPFLAGS += -I$(top_srcdir)/include
AM_CPPFLAGS += -I$(top_srcdir)/control

# FIXME the "../" is ugly
AM_CPPFLAGS += -I../$(top_srcdir)/xen-4.2.hg/tools/include

# FIXME not sure this is the right way to access xenstore stuff
AM_CPPFLAGS += -I$(top_srcdir)/../xen-4.2.hg/tools/xenstore

AM_CPPFLAGS += -I$(top_srcdir)/../xen-4.2.hg/stubdom/ioemu/hw

bin_PROGRAMS = tapback

tapback_SOURCES  = log.c
tapback_SOURCES += xenstore.c
tapback_SOURCES += frontend.c
tapback_SOURCES += backend.c
tapback_SOURCES += tapback.h
tapback_SOURCES += tapback.c

tapback_LDADD  = -L$(top_srcdir)/../xen-4.2.hg/tools/xenstore
tapback_LDADD += -lxenstore
tapback_LDADD += $(top_srcdir)/control/libblktapctl.la

#tapback_la_LDFLAGS = 
