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
  PTHREAD_OPTION:=-pthread
  LIBS:=$(LIBS) -lfuse
  CPPFLAGS:=$(CPPFLAGS) -D_FILE_OFFSET_BITS=64
endif

BINARIES:=fatfs
MANPAGES8:=fatfs.8

all: $(BINARIES)

.PHONY: all install clean

install: all
	mkdir -m 755 -p $(DESTDIR)$(SBINDIR)
	install -m 755 $(BINARIES) $(DESTDIR)$(SBINDIR)
	mkdir -m 755 -p $(DESTDIR)$(MANDIR)/man8
	install -m 644 $(MANPAGES8) $(DESTDIR)$(MANDIR)/man8

fatfs: *.cpp *.h
	$(CXX) $(PTHREAD_OPTION) -std=gnu++11 $(CPPFLAGS) $(CXXFLAGS) *.cpp -o $@ $(LIBS)

clean:
	rm -f $(BINARIES) *.o
