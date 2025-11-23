include ../build-aux/platform.mak
include ../build-aux/compiler.mak
include ../build-aux/version.mak
include ../build-aux/dirs.mak

OPTLEVEL?=$(DEFAULT_OPTLEVEL)
CFLAGS?=$(OPTLEVEL)

CFLAGS:=$(CFLAGS) -Wall -Wextra
CPPFLAGS:=$(CPPFLAGS) -DVERSIONSTR=\"$(VERSION)\"

BINARY:=sf

all: $(BINARY)

.PHONY: all install clean

install: all
	mkdir -m 755 -p $(DESTDIR)$(BINDIR)
	install -m 755 $(BINARY) $(DESTDIR)$(BINDIR)
	install -m 755 sfnc $(DESTDIR)$(BINDIR)
	install -m 755 sfncd $(DESTDIR)$(BINDIR)
	mkdir -m 755 -p $(DESTDIR)$(MANDIR)/man1
	install -m 644 sf.1 $(DESTDIR)$(MANDIR)/man1/sf.1
	install -m 644 sfnc.1 $(DESTDIR)$(MANDIR)/man1/sfnc.1
	install -m 644 sfncd.1 $(DESTDIR)$(MANDIR)/man1/sfncd.1

$(BINARY): $(BINARY).c
	$(CC) $(CFLAGS) $(CPPFLAGS) -std=gnu11 $< -o $@

clean:
	rm -f $(BINARY) *.o
