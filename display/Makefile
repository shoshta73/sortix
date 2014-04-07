SOFTWARE_MEANT_FOR_SORTIX=1
include ../build-aux/platform.mak
include ../build-aux/compiler.mak
include ../build-aux/version.mak
include ../build-aux/dirs.mak

OPTLEVEL?=-g -O2
CFLAGS?=$(OPTLEVEL)

CFLAGS:=$(CFLAGS) -Wall -Wextra
CPPFLAGS:=$(CPPFLAGS) -DVERSIONSTR=\"$(VERSION)\" -Iinclude

PROGRAMS=\
display \
nyan \
program \

LIBRARIES=\
libdisplay.a

DISPLAY_OBJS=\
connection.o \
display-code.o \
display.o \
framebuffer.o \
pixel.o \
server.o \
vgafont.o \
window.o \

all: $(PROGRAMS) $(LIBRARIES)

.PHONY: all install uninstall clean

install: all
	mkdir -p $(DESTDIR)$(BINDIR)
	install $(PROGRAMS) $(DESTDIR)$(BINDIR)
	mkdir -p $(DESTDIR)$(LIBDIR)
	install $(LIBRARIES) $(DESTDIR)$(LIBDIR)
	mkdir -p $(DESTDIR)$(INCLUDEDIR)
	cp -RTv include $(DESTDIR)$(INCLUDEDIR)

$(PROGRAMS): $(LIBRARIES)

display: $(DISPLAY_OBJS)
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) $(DISPLAY_OBJS) -o $@

%: %.c
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) $< -o $@ $(LIBRARIES)

%.o: %.c
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) -c $< -o $@

libdisplay.a: libdisplay.o
	$(AR) rcs $@ libdisplay.o

clean:
	rm -f $(PROGRAMS) $(LIBRARIES) *.o *.a
