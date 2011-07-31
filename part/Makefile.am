
AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror

AM_CPPFLAGS  = -D_GNU_SOURCE
AM_CPPFLAGS += -I../include

sbin_PROGRAMS = part-util

part_util_SOURCES  = part-util.c
part_util_SOURCES += partition.c
part_util_SOURCES += partition.h

dist_sbin_SCRIPTS  = vhdpartx
