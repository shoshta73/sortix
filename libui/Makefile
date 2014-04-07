SOFTWARE_MEANT_FOR_SORTIX=1
include ../build-aux/platform.mak
include ../build-aux/compiler.mak
include ../build-aux/dirs.mak

OPTLEVEL?=-g -O2
CFLAGS?=$(OPTLEVEL)

CFLAGS:=$(CFLAGS) -Wall -Wextra
CPPFLAGS:=$(CPPFLAGS) -Iinclude

LIBRARY=libui.a

OBJS=\
framebuffer.o \
pixel.o \
vgafont.o \

all: $(LIBRARY)

.PHONY: all install clean

install: all
	mkdir -p $(DESTDIR)$(LIBDIR)
	cp $(LIBRARY) $(DESTDIR)$(LIBDIR)
	mkdir -p $(DESTDIR)$(INCLUDEDIR)
	cp -RTv include $(DESTDIR)$(INCLUDEDIR)

%.o: %.c
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(LIBRARY): $(OBJS)
	$(AR) rcs $@ $(OBJS)

clean:
	rm -f $(LIBRARY) *.o *.a
