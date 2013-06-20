XS_INCLUDE ?= /usr/include
XS_LIB     ?= -lxenstore

PREFIX ?= /opt/xensource/sm/snapwatchd
INITDIR ?= /etc/rc.d/init.d
DESTDIR ?= 

PYTHON_INCLUDE ?= /usr/include/python2.4

CFLAGS ?= -O2
CFLAGS += -I$(PYTHON_INCLUDE) -I$(XS_INCLUDE)

SWIG ?= swig

.PHONY: all
all: _xslib.so
	@ :

.PHONY: install
install: _xslib.so xslib.py snapwatchd
	mkdir -p $(DESTDIR)$(PREFIX)
	install -m 644 $^ $(DESTDIR)$(PREFIX)
	chmod +x $(DESTDIR)$(PREFIX)/snapwatchd
	mkdir -p $(DESTDIR)$(INITDIR)
	cp snapwatchd.init $(DESTDIR)$(INITDIR)/snapwatchd
	chmod +x $(DESTDIR)$(INITDIR)/snapwatchd

%.py %_wrap.c: %.i
	$(SWIG) -python $<

_xslib.so: xslib.py xslib_wrap.o xslib.o
	$(CC) $(CFLAGS) -shared xslib.o xslib_wrap.o $(XS_LIB) -o $@

.PHONY: clean
clean:
	rm -f _xslib.so xslib_wrap.c xslib_wrap.o xslib.py xslib.o

