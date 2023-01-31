CFLAGS:=$(filter-out -fsanitize=leak,$(LDFLAGS))
LDFLAGS:=$(filter-out -static-liblsan,$(LDFLAGS))

AM_CFLAGS  = -Wall
AM_CFLAGS += -Werror

AM_CPPFLAGS = -I../include

noinst_LTLIBRARIES = libwrappers.la

libwrappers_la_SOURCES = wrappers.c
