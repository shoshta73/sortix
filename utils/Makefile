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
chkblayout \
chmod \
chown \
chvideomode \
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
halt \
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
nl \
pager \
passwd \
ps \
pstree \
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
touch \
tr \
true \
tty \
type \
uname \
uniq \
uptime \
wc \
which \
yes \
poweroff \
reboot \

BINARIES=\
$(BINARIES_EXCEPT_INSTALL) \
xinstall

MANPAGES1=\
chkblayout.1 \
chvideomode.1 \
kernelinfo.1 \
logname.1 \
memstat.1 \
pager.1 \
passwd.1 \
readlink.1 \
sort.1 \
uname.1 \

SBINS=\
chroot \
unmount \

MANPAGES8=\
chroot.8 \
halt.8 \
poweroff.8 \
reboot.8 \
unmount.8 \

all: $(BINARIES) $(SBINS)

.PHONY: all install clean

install: all
	mkdir -p $(DESTDIR)$(BINDIR)
	install $(BINARIES_EXCEPT_INSTALL) $(DESTDIR)$(BINDIR)
	install xinstall $(DESTDIR)$(BINDIR)/install
	mkdir -p $(DESTDIR)$(SBINDIR)
	install $(SBINS) $(DESTDIR)$(SBINDIR)
	mkdir -p $(DESTDIR)$(MANDIR)/man1
	cp $(MANPAGES1) $(DESTDIR)$(MANDIR)/man1
	mkdir -p $(DESTDIR)$(MANDIR)/man8
	cp $(MANPAGES8) $(DESTDIR)$(MANDIR)/man8

head: tail.c

xinstall: cp.c

%: %.c
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) $< -o $@

clean:
	rm -f $(BINARIES) $(SBINS) *.o
