include ../build-aux/platform.mak
include ../build-aux/dirs.mak

all:

.PHONY: all install clean

clean:

install: all
	mkdir -m 755 -p $(DESTDIR)$(SBINDIR)
	install -m 755 update-initrd $(DESTDIR)$(SBINDIR)
	mkdir -m 755 -p $(DESTDIR)$(MANDIR)/man8
	install -m 644 update-initrd.8 $(DESTDIR)$(MANDIR)/man8/update-initrd.8
