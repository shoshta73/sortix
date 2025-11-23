include ../build-aux/platform.mak
include ../build-aux/compiler.mak
include ../build-aux/version.mak
include ../build-aux/dirs.mak

OPTLEVEL?=$(DEFAULT_OPTLEVEL)
CXXFLAGS?=$(OPTLEVEL)

CPPFLAGS:=$(CPPFLAGS) -DVERSIONSTR=\"$(VERSION)\"
CXXFLAGS:=$(CXXFLAGS) -Wall -Wextra -fno-exceptions -fno-rtti -fcheck-new $(NOSTDLIBCXX)

LIBS:=$(LIBS)

ifeq ($(HOST_IS_SORTIX),0)
  LIBS:=$(LIBS) -lfuse
  CPPFLAGS:=$(CPPFLAGS) -D_FILE_OFFSET_BITS=64
endif

BINARIES:=iso9660fs
MANPAGES8:=iso9660fs.8

all: $(BINARIES)

.PHONY: all install clean

install: all
	mkdir -m 755 -p $(DESTDIR)$(SBINDIR)
	install -m 755 $(BINARIES) $(DESTDIR)$(SBINDIR)
	mkdir -m 755 -p $(DESTDIR)$(MANDIR)/man8
	install -m 644 $(MANPAGES8) $(DESTDIR)$(MANDIR)/man8

iso9660fs: *.cpp *.h
	$(CXX) -std=gnu++11 $(CPPFLAGS) $(CXXFLAGS) *.cpp -o $@ $(LIBS)

clean:
	rm -f $(BINARIES) *.o
