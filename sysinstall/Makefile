SOFTWARE_MEANT_FOR_SORTIX=1
include ../build-aux/platform.mak
include ../build-aux/compiler.mak
include ../build-aux/version.mak
include ../build-aux/dirs.mak

OPTLEVEL?=$(DEFAULT_OPTLEVEL)
CFLAGS?=$(OPTLEVEL)

CFLAGS:=$(CFLAGS) -Wall -Wextra
CPPFLAGS:=$(CPPFLAGS) -DVERSIONSTR=\"$(VERSION)\"

MAIN_OBJS=\
sysinstall.o \
sysmerge.o \
sysupgrade.o \

UTIL_OBJS=\
conf.o \
devices.o \
execute.o \
fileops.o \
hooks.o \
interactive.o \
manifest.o \
release.o \
string_array.o \

OBJS=$(MAIN_OBJS) $(UTIL_OBJS)

SYSINSTALL_DEPS=conf devices execute fileops interactive manifest release string_array
SYSMERGE_DEPS=conf fileops execute hooks manifest release string_array
SYSUPGRADE_DEPS=conf devices execute fileops hooks interactive manifest release string_array

SYSINSTALL_OBJS:=sysinstall.o $(SYSINSTALL_DEPS:=.o)
SYSMERGE_OBJS:=sysmerge.o $(SYSMERGE_DEPS:=.o)
SYSUPGRADE_OBJS:=sysupgrade.o $(SYSUPGRADE_DEPS:=.o)

all: sysinstall sysmerge sysupgrade

.PHONY: all install clean

install: all
	mkdir -p $(DESTDIR)$(SBINDIR)
	install sysinstall $(DESTDIR)$(SBINDIR)
	install sysmerge $(DESTDIR)$(SBINDIR)
	install sysupgrade $(DESTDIR)$(SBINDIR)
	mkdir -p $(DESTDIR)$(MANDIR)/man8
	cp sysinstall.8 $(DESTDIR)$(MANDIR)/man8/sysinstall.8
	cp sysmerge.8 $(DESTDIR)$(MANDIR)/man8/sysmerge.8
	cp sysupgrade.8 $(DESTDIR)$(MANDIR)/man8/sysupgrade.8
	mkdir -p $(DESTDIR)$(DATADIR)/sysinstall/hooks
	# TODO: After releasing Sortix 1.1, remove this compatibility.
	touch $(DESTDIR)$(DATADIR)/sysinstall/hooks/sortix-1.1-random-seed
	touch $(DESTDIR)$(DATADIR)/sysinstall/hooks/sortix-1.1-tix-manifest-mode
	touch $(DESTDIR)$(DATADIR)/sysinstall/hooks/sortix-1.1-leaked-files
	touch $(DESTDIR)$(DATADIR)/sysinstall/hooks/sortix-1.1-init

sysinstall: $(SYSINSTALL_OBJS)
	$(CC) $(SYSINSTALL_OBJS) -o $@ -lmount

sysmerge: $(SYSMERGE_OBJS)
	$(CC) $(SYSMERGE_OBJS) -o $@ -lmount

sysupgrade: $(SYSUPGRADE_OBJS)
	$(CC) $(SYSUPGRADE_OBJS) -o $@ -lmount

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -std=gnu11 -c $< -o $@

sysinstall.o: $(SYSINSTALL_DEPS:=.h)
sysmerge.o: $(SYSMERGE_DEPS:=.h)
sysupgrade.o: $(SYSUPGRADE_DEPS:=.h)
conf.o: conf.h
devices.o: devices.h
execute.o: execute.h
fileops.o: fileops.h string_array.h
string_array.o: string_array.h
hooks.o: fileops.h manifest.h release.h string_array.h
interactive.o: interactive.h execute.h
manifest.o: manifest.h fileops.h string_array.h
release.o: release.h

clean:
	rm -f sysinstall sysmerge sysupgrade $(OBJS)
