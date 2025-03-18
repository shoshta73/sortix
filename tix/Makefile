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
tix-create \
tix-execdiff \
tix-execpatch \
tix-install \
tix-rmdiff \
tix-rmpatch \
tix-vars \
tix-uninstall \

PROGRAMS:=\
$(BINARIES) \
tix-autoupgrade \
tix-clean \
tix-eradicate-libtool-la \
tix-fetch \
tix-iso-add \
tix-iso-bootconfig \
tix-iso-liveconfig \
tix-list-packages \
tix-metabuild \
tix-port \
tix-release \
tix-repository \
tix-upgrade \

MANPAGES8=\
tix.8 \
tix-autoupgrade.8 \
tix-build.8 \
tix-clean.8 \
tix-create.8 \
tix-eradicate-libtool-la.8 \
tix-execdiff.8 \
tix-execpatch.8 \
tix-fetch.8 \
tix-install.8 \
tix-iso-add.8 \
tix-iso-bootconfig.8 \
tix-iso-liveconfig.8 \
tix-list-packages.8 \
tix-metabuild.8 \
tix-port.8 \
tix-release.8 \
tix-repository.8 \
tix-rmdiff.8 \
tix-rmpatch.8 \
tix-vars.8 \
tix-uninstall.8 \
tix-upgrade.8 \

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
	# TODO: After releasing Sortix 1.1, delete tix-collection compatibility.
	ln -f $(DESTDIR)$(SBINDIR)/tix-create $(DESTDIR)$(SBINDIR)/tix-collection

clean:
	rm -f $(BINARIES)
