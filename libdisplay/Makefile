SOFTWARE_MEANT_FOR_SORTIX=1
include ../build-aux/platform.mak
include ../build-aux/compiler.mak
include ../build-aux/dirs.mak

OPTLEVEL?=$(DEFAULT_OPTLEVEL)
CFLAGS?=$(OPTLEVEL)

CFLAGS:=$(CFLAGS) -Wall -Wextra
CPPFLAGS:=$(CPPFLAGS) -Iinclude

LIBRARY=libdisplay.a

OBJS=\
libdisplay.o \

all: $(LIBRARY)

.PHONY: all install clean

install: all
	mkdir -m 755 -p $(DESTDIR)$(LIBDIR)
	install -m 644 $(LIBRARY) $(DESTDIR)$(LIBDIR)
	mkdir -m 755 -p $(DESTDIR)$(INCLUDEDIR)
	cp -RT include $(DESTDIR)$(INCLUDEDIR)
	for file in `cd include && find . | sort`; do chmod o=u-w,g=o "$(DESTDIR)$(INCLUDEDIR)/$$file"; done

%.o: %.c
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(LIBRARY): $(OBJS)
	$(AR) rcs $@ $(OBJS)

clean:
	rm -f $(LIBRARY) *.o *.a
