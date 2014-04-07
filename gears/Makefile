include ../build-aux/compiler.mak
include ../build-aux/version.mak
include ../build-aux/dirs.mak

OPTLEVEL?=-g -O2
CXXFLAGS?=$(OPTLEVEL)

CPPFLAGS:=$(CPPFLAGS)
CXXFLAGS:=$(CXXFLAGS) -Wall -Wextra -fno-exceptions -fno-rtti

BINARY:=gears
LIBS:=-lOSMesa -ldisplay

all: $(BINARY)

.PHONY: all install uninstall clean

%: %.cpp
	$(CXX) -std=gnu++11 $(CPPFLAGS) $(CXXFLAGS) $< -o $@ $(LIBS)

install: all
	mkdir -p $(DESTDIR)$(BINDIR)
	install $(BINARY) $(DESTDIR)$(BINDIR)

clean:
	rm -f $(BINARY)
