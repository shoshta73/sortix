SOFTWARE_MEANT_FOR_SORTIX=1
include ../build-aux/platform.mak
include ../build-aux/compiler.mak
include ../build-aux/dirs.mak

OPTLEVEL?=$(DEFAULT_OPTLEVEL)
CFLAGS?=$(OPTLEVEL)

CFLAGS += -Wall -Wextra

BINARIES = chvideomode
MANPAGES1 = chvideomode.1

LIBS = -ldisplay

all: $(BINARIES)

.PHONY: all install clean

install: all
	mkdir -m 755 -p $(DESTDIR)$(BINDIR)
	install -m 755 $(BINARIES) $(DESTDIR)$(BINDIR)
	mkdir -m 755 -p $(DESTDIR)$(MANDIR)/man1
	install -m 644 $(MANPAGES1) $(DESTDIR)$(MANDIR)/man1

%: %.c
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LIBS)

clean:
	rm -f $(BINARIES)
