PREFIX ?= /opt/xensource/libexec
DESTDIR ?= 
DEBUGDIR ?= /opt/xensource/debug


OPTS := -D _GNU_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE -Wall

SRC := dcopy.c
SRC += atomicio.c

BIN := dcopy

all: dcopy tp

dcopy: dcopy.c
	$(CC) $(OPTS) $(SRC) -o $(BIN)

tp: tp.c
	$(CC) $(OPTS) tp.c -o tp

test: tp
	./test.sh

install: install_dcopy install_tp

install_dcopy: dcopy
	mkdir -p $(DESTDIR)$(PREFIX)
	install -m 755 $^ $(DESTDIR)$(PREFIX)

install_tp: tp
	mkdir -p $(DESTDIR)$(DEBUGDIR)
	install -m 755 $^ $(DESTDIR)$(DEBUGDIR)	

clean: 
	rm -f dcopy tp *.o source_img dest_img
