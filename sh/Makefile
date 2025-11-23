SOFTWARE_MEANT_FOR_SORTIX=1
include ../build-aux/platform.mak
include ../build-aux/compiler.mak
include ../build-aux/version.mak
include ../build-aux/dirs.mak

OPTLEVEL?=$(DEFAULT_OPTLEVEL)
CFLAGS?=$(OPTLEVEL)

CPPFLAGS:=$(CPPFLAGS) -DVERSIONSTR=\"$(VERSION)\"
CFLAGS:=$(CFLAGS) -Wall -Wextra

BINARIES:=sh sortix-sh

SORTIX_SH_SRCS=\
editline.c \
sh.c \
showline.c \
util.c

all: $(BINARIES)

.PHONY: all install clean

install: all
	mkdir -m 755 -p $(DESTDIR)$(BINDIR)
	install -m 755 $(BINARIES) $(DESTDIR)$(BINDIR)
	mkdir -m 755 -p $(DESTDIR)$(MANDIR)/man1
	install -m 644 sh.1 $(DESTDIR)$(MANDIR)/man1/sh.1
	ln -sf sh.1 $(DESTDIR)$(MANDIR)/man1/sortix-sh.1
	mkdir -m 755 -p $(DESTDIR)$(MANDIR)/man5
	install -m 644 profile.5 $(DESTDIR)$(MANDIR)/man5/profile.5
	install -m 644 proper-sh.5 $(DESTDIR)$(MANDIR)/man5/proper-sh.5
	install -m 644 shrc.5 $(DESTDIR)$(MANDIR)/man5/shrc.5

sortix-sh: $(SORTIX_SH_SRCS) *.h
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) $(SORTIX_SH_SRCS) -o $@

sh: proper-sh.c
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) $< -o $@

clean:
	rm -f $(BINARIES) *.o
