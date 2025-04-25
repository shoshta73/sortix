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

BINARIES:=extfs
MANPAGES8:=extfs.8

all: $(BINARIES)

.PHONY: all install clean

install: all
	mkdir -p $(DESTDIR)$(SBINDIR)
	install $(BINARIES) $(DESTDIR)$(SBINDIR)
	mkdir -p $(DESTDIR)$(MANDIR)/man8
	install $(MANPAGES8) $(DESTDIR)$(MANDIR)/man8

extfs: *.cpp *.h
	$(CXX) $(PTHREAD_OPTION) -std=gnu++11 $(CPPFLAGS) $(CXXFLAGS) *.cpp -o $@ $(LIBS)

clean:
	rm -f $(BINARIES) *.o
