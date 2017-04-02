include ../build-aux/platform.mak
include ../build-aux/compiler.mak
include ../build-aux/version.mak
include ../build-aux/dirs.mak

OPTLEVEL?=$(DEFAULT_OPTLEVEL)
CFLAGS?=$(OPTLEVEL)

CFLAGS:=$(CFLAGS) -Wall -Wextra
CPPFLAGS:=$(CPPFLAGS) -DVERSIONSTR=\"$(VERSION)\"

ifeq ($(HOST_IS_SORTIX),0)
  CPPFLAGS+=-D_GNU_SOURCE
endif

BINARIES:=\
tix \
tix-build \
tix-collection \
tix-execdiff \
tix-execpatch \
tix-install \
tix-rmdiff \
tix-rmpatch \
tix-vars \

PROGRAMS:=\
$(BINARIES) \
tix-clean \
tix-eradicate-libtool-la \
tix-fetch \
tix-iso-add \
tix-iso-bootconfig \
tix-iso-liveconfig \
tix-port \
tix-upgrade \

MANPAGES8=\
tix-build.8 \
tix-eradicate-libtool-la.8 \
tix-execdiff.8 \
tix-execpatch.8 \
tix-iso-add.8 \
tix-iso-bootconfig.8 \
tix-iso-liveconfig.8 \
tix-port.8 \
tix-rmdiff.8 \
tix-rmpatch.8 \
tix-vars.8 \

all: $(PROGRAMS)

.PHONY: all install clean

%: %.c util.h
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) $< -o $@

$(DESTDIR)$(SBINDIR):
	mkdir -p $@

install: all $(DESTDIR)$(SBINDIR)
	install $(PROGRAMS) $(DESTDIR)$(SBINDIR)
	mkdir -p $(DESTDIR)$(MANDIR)/man8
	mkdir -p $(DESTDIR)$(MANDIR)/man8
	cp $(MANPAGES8) $(DESTDIR)$(MANDIR)/man8

clean:
	rm -f $(BINARIES)
