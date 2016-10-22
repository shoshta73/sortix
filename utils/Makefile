SOFTWARE_MEANT_FOR_SORTIX=1
include ../build-aux/platform.mak
include ../build-aux/compiler.mak
include ../build-aux/version.mak
include ../build-aux/dirs.mak

OPTLEVEL?=$(DEFAULT_OPTLEVEL)
CFLAGS?=$(OPTLEVEL)

CFLAGS:=$(CFLAGS) -Wall -Wextra
CPPFLAGS:=$(CPPFLAGS) -DVERSIONSTR=\"$(VERSION)\"

BINARIES_EXCEPT_INSTALL:=\
basename \
cat \
chgrp \
chmod \
chown \
clear \
colormake \
column \
command-not-found \
cp \
date \
df \
dirname \
du \
echo \
env \
expr \
false \
find \
getaddrinfo \
getconf \
head \
help \
id \
kernelinfo \
kill \
ln \
logname \
ls \
memstat \
mkdir \
mktemp \
mv \
nc \
nl \
pager \
passwd \
ps \
pstree \
pty \
pwd \
readlink \
realpath \
rm \
rmdir \
sleep \
sort \
stat \
stty \
tail \
tee \
time \
timeout \
touch \
tr \
true \
tty \
uname \
uniq \
uptime \
wall \
wc \
which \
yes \

BINARIES=\
$(BINARIES_EXCEPT_INSTALL) \
xinstall

MANPAGES1=\
getconf.1 \
kernelinfo.1 \
logname.1 \
nc.1 \
memstat.1 \
pager.1 \
passwd.1 \
readlink.1 \
sort.1 \
uname.1 \
wall.1 \

SBINS=\
chroot \
getty \
halt \
poweroff \
reboot \
shutdown \
unmount \

MANPAGES8=\
chroot.8 \
getty.8 \
halt.8 \
poweroff.8 \
reboot.8 \
shutdown.8 \
unmount.8 \

all: $(BINARIES) $(SBINS)

.PHONY: all install clean

install: all
	mkdir -m 755 -p $(DESTDIR)$(BINDIR)
	install -m 755 $(BINARIES_EXCEPT_INSTALL) $(DESTDIR)$(BINDIR)
	install -m 755 xinstall $(DESTDIR)$(BINDIR)/install
	mkdir -m 755 -p $(DESTDIR)$(SBINDIR)
	install -m 755 $(SBINS) $(DESTDIR)$(SBINDIR)
	mkdir -m 755 -p $(DESTDIR)$(MANDIR)/man1
	install -m 644 $(MANPAGES1) $(DESTDIR)$(MANDIR)/man1
	mkdir -m 755 -p $(DESTDIR)$(MANDIR)/man8
	install -m 644 $(MANPAGES8) $(DESTDIR)$(MANDIR)/man8

head: tail.c

xinstall: cp.c

%: %.c
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) $< -o $@

clean:
	rm -f $(BINARIES) $(SBINS) *.o
