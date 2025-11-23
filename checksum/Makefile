SOFTWARE_MEANT_FOR_SORTIX=1
include ../build-aux/platform.mak
include ../build-aux/compiler.mak
include ../build-aux/version.mak
include ../build-aux/dirs.mak

OPTLEVEL?=$(DEFAULT_OPTLEVEL)
CFLAGS?=$(OPTLEVEL)

CFLAGS += -Wall -Wextra

all: checksum

.PHONY: all install clean

install: all
	mkdir -m 755 -p $(DESTDIR)$(BINDIR)
	install -m 755 checksum $(DESTDIR)$(BINDIR)
	ln -f $(DESTDIR)$(BINDIR)/checksum $(DESTDIR)$(BINDIR)/sha224sum
	ln -f $(DESTDIR)$(BINDIR)/checksum $(DESTDIR)$(BINDIR)/sha256sum
	ln -f $(DESTDIR)$(BINDIR)/checksum $(DESTDIR)$(BINDIR)/sha384sum
	ln -f $(DESTDIR)$(BINDIR)/checksum $(DESTDIR)$(BINDIR)/sha512sum
	mkdir -m 755 -p $(DESTDIR)$(MANDIR)/man1
	install -m 644 checksum.1 $(DESTDIR)$(MANDIR)/man1/checksum.1
	ln -f $(DESTDIR)$(MANDIR)/man1/checksum.1 $(DESTDIR)$(MANDIR)/man1/sha224sum.1
	ln -f $(DESTDIR)$(MANDIR)/man1/checksum.1 $(DESTDIR)$(MANDIR)/man1/sha256sum.1
	ln -f $(DESTDIR)$(MANDIR)/man1/checksum.1 $(DESTDIR)$(MANDIR)/man1/sha384sum.1
	ln -f $(DESTDIR)$(MANDIR)/man1/checksum.1 $(DESTDIR)$(MANDIR)/man1/sha512sum.1

%: %.c
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) $< -o $@

clean:
	rm -f checksum
