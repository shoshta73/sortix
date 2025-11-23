include ../build-aux/platform.mak
include ../build-aux/compiler.mak
include ../build-aux/version.mak
include ../build-aux/dirs.mak

OPTLEVEL?=$(DEFAULT_OPTLEVEL)
CFLAGS?=$(OPTLEVEL)

CFLAGS:=$(CFLAGS) -Wall -Wextra

BINARIES=\
init \
service \

MANPAGES8=\
init.8 \
service.8 \

all: $(BINARIES)

.PHONY: all install clean

%: %.c
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) $< -o $@ -lmount $(LIBS)

install: all
	mkdir -m 755 -p $(DESTDIR)$(SBINDIR)
	install -m 755 $(BINARIES) $(DESTDIR)$(SBINDIR)
	mkdir -m 755 -p $(DESTDIR)$(MANDIR)/man8
	install -m 644 $(MANPAGES8) $(DESTDIR)$(MANDIR)/man8

clean:
	rm -f $(BINARIES) $(OBJS) *.o
