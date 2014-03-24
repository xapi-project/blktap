XS_INCLUDE ?= /usr/include

PREFIX ?= /opt/xensource/sm/snapwatchd
INITDIR ?= /etc/rc.d/init.d
DESTDIR ?= 

.PHONY: install
install: xslib.py snapwatchd snapdebug.py
	mkdir -p $(DESTDIR)$(PREFIX)
	install -m 644 $^ $(DESTDIR)$(PREFIX)
	chmod +x $(DESTDIR)$(PREFIX)/snapwatchd
	chmod +x $(DESTDIR)$(PREFIX)/xslib.py
	chmod +x $(DESTDIR)$(PREFIX)/snapdebug.py
	mkdir -p $(DESTDIR)$(INITDIR)
	cp snapwatchd.init $(DESTDIR)$(INITDIR)/snapwatchd
	chmod +x $(DESTDIR)$(INITDIR)/snapwatchd

.PHONY: clean
clean:
	rm -f xslib.pyc xslib.pyo snapdebug.pyc snapdebug.pyo

