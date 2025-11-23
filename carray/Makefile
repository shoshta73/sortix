include ../build-aux/platform.mak
include ../build-aux/compiler.mak
include ../build-aux/version.mak
include ../build-aux/dirs.mak

OPTLEVEL?=$(DEFAULT_OPTLEVEL)
CFLAGS?=$(OPTLEVEL)

CFLAGS:=$(CFLAGS) -Wall -Wextra

ifeq ($(HOST_IS_SORTIX),0)
  CPPFLAGS+=-D_GNU_SOURCE
endif

BINARY:=carray

all: $(BINARY)

.PHONY: all install clean

install: all
	mkdir -m 755 -p $(DESTDIR)$(BINDIR)
	install -m 755 $(BINARY) $(DESTDIR)$(BINDIR)
	mkdir -m 755 -p $(DESTDIR)$(MANDIR)/man1
	install -m 644 carray.1 $(DESTDIR)$(MANDIR)/man1/carray.1

%: %.c
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) $< -o $@

clean:
	rm -f $(BINARY) *.o
