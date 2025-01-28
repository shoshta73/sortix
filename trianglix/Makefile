SOFTWARE_MEANT_FOR_SORTIX=1
include ../build-aux/platform.mak
include ../build-aux/compiler.mak
include ../build-aux/version.mak
include ../build-aux/dirs.mak

OPTLEVEL?=$(DEFAULT_OPTLEVEL)
CXXFLAGS?=$(OPTLEVEL)

CXXFLAGS:=$(CXXFLAGS) -std=gnu++11 -Wall -Wextra -fno-exceptions -fno-rtti -fcheck-new $(NOSTDLIBCXX)

BINARY:=trianglix

all: $(BINARY)

.PHONY: all install clean

install: all
	mkdir -p $(DESTDIR)$(BINDIR)
	install $(BINARY) $(DESTDIR)$(BINDIR)

%: %.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ $(LIBS)

clean:
	rm -f $(BINARY) *.o
