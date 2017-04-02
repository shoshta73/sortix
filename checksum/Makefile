include ../build-aux/platform.mak
include ../build-aux/compiler.mak
include ../build-aux/version.mak
include ../build-aux/dirs.mak

OPTLEVEL?=$(DEFAULT_OPTLEVEL)
CFLAGS?=$(OPTLEVEL)

CFLAGS:=$(CFLAGS) -Wall -Wextra

all: checksum

.PHONY: all install clean

%: %.c
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LIBS)

install: all
	mkdir -p $(DESTDIR)$(BINDIR)
	install checksum $(DESTDIR)$(BINDIR)
	ln -f $(DESTDIR)$(BINDIR)/checksum $(DESTDIR)$(BINDIR)/sha224sum
	ln -f $(DESTDIR)$(BINDIR)/checksum $(DESTDIR)$(BINDIR)/sha256sum
	ln -f $(DESTDIR)$(BINDIR)/checksum $(DESTDIR)$(BINDIR)/sha384sum
	ln -f $(DESTDIR)$(BINDIR)/checksum $(DESTDIR)$(BINDIR)/sha512sum
	mkdir -p $(DESTDIR)$(MANDIR)/man1
	#cp checksum.1 $(DESTDIR)$(MANDIR)/man1/checksum.1
	#ln -f $(DESTDIR)$(MANDIR)/man1/checksum.1 $(DESTDIR)$(MANDIR)/man1/sha224sum.1
	#ln -f $(DESTDIR)$(MANDIR)/man1/checksum.1 $(DESTDIR)$(MANDIR)/man1/sha256sum.1
	#ln -f $(DESTDIR)$(MANDIR)/man1/checksum.1 $(DESTDIR)$(MANDIR)/man1/sha384sum.1
	#ln -f $(DESTDIR)$(MANDIR)/man1/checksum.1 $(DESTDIR)$(MANDIR)/man1/sha512sum.1

clean:
	rm -f checksum
