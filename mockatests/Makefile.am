# ACLOCAL_AMFLAGS = -I m4

CFLAGS:=$(filter-out -fsanitize=leak,$(CFLAGS))
LDFLAGS:=$(filter-out -static-liblsan,$(LDFLAGS))

SUBDIRS  = wrappers
SUBDIRS += drivers
SUBDIRS += cbt
SUBDIRS += control
SUBDIRS += vhd
