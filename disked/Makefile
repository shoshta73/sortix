SOFTWARE_MEANT_FOR_SORTIX=1
include ../build-aux/platform.mak
include ../build-aux/compiler.mak
include ../build-aux/version.mak
include ../build-aux/dirs.mak

OPTLEVEL?=$(DEFAULT_OPTLEVEL)
CFLAGS?=$(OPTLEVEL)

CFLAGS:=$(CFLAGS) -Wall -Wextra
CPPFLAGS:=$(CPPFLAGS) -DVERSIONSTR=\"$(VERSION)\"

BINARY:=disked

all: $(BINARY)

.PHONY: all install clean

install: all
	mkdir -m 755 -p $(DESTDIR)$(SBINDIR)
	install -m 755 $(BINARY) $(DESTDIR)$(SBINDIR)
	mkdir -m 755 -p $(DESTDIR)$(MANDIR)/man8
	install -m 644 disked.8 $(DESTDIR)$(MANDIR)/man8

$(BINARY): $(BINARY).c
	$(CC) $(CFLAGS) $(CPPFLAGS) -std=gnu11 $< -o $@ -lmount

clean:
	rm -f $(BINARY)
