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
chroot \
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
head \
help \
id \
kernelinfo \
kill \
ln \
ls \
memstat \
mkdir \
mktemp \
mv \
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
tail \
tee \
time \
touch \
tr \
true \
type \
uname \
uniq \
unmount \
uptime \
wc \
which \
yes \

BINARIES=\
$(BINARIES_EXCEPT_INSTALL) \
xinstall

MANPAGES=\
chkblayout.1 \
kernelinfo.1 \
passwd.1 \
readlink.1 \

all: $(BINARIES)

.PHONY: all install clean

install: all
	mkdir -p $(DESTDIR)$(BINDIR)
	install $(BINARIES_EXCEPT_INSTALL) $(DESTDIR)$(BINDIR)
	install xinstall $(DESTDIR)$(BINDIR)/install
	mkdir -p $(DESTDIR)$(MANDIR)/man1
	cp $(MANPAGES) $(DESTDIR)$(MANDIR)/man1

%: %.c
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) $< -o $@

clean:
	rm -f $(BINARIES) *.o
