INITDIR ?= /etc/rc.d/init.d
DESTDIR ?= 

.PHONY: all
all:

.PHONY: install
install: mpathroot.init
	cp mpathroot.init $(DESTDIR)$(INITDIR)/mpathroot
	chmod +x $(DESTDIR)$(INITDIR)/mpathroot

.PHONY: clean
clean:

