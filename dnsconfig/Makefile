SOFTWARE_MEANT_FOR_SORTIX=1
include ../build-aux/platform.mak
include ../build-aux/compiler.mak
include ../build-aux/version.mak
include ../build-aux/dirs.mak

OPTLEVEL?=$(DEFAULT_OPTLEVEL)
CFLAGS?=$(OPTLEVEL)

CFLAGS += -Wall -Wextra

BINARIES = dnsconfig
MANPAGES8 = dnsconfig.8

all: $(BINARIES)

.PHONY: all install clean

install: all
	mkdir -m 755 -p $(DESTDIR)$(SBINDIR)
	install -m 755 $(BINARIES) $(DESTDIR)$(SBINDIR)
	mkdir -m 755 -p $(DESTDIR)$(MANDIR)/man8
	install -m 644 $(MANPAGES8) $(DESTDIR)$(MANDIR)/man8

%: %.c
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) $< -o $@

clean:
	rm -f $(BINARIES)

