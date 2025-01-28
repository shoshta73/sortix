SOFTWARE_MEANT_FOR_SORTIX=1
include ../build-aux/platform.mak
include ../build-aux/compiler.mak
include ../build-aux/dirs.mak

OPTLEVEL?=$(DEFAULT_OPTLEVEL)
CFLAGS?=$(OPTLEVEL)

CFLAGS:=$(CFLAGS) -Wall -Wextra

PROGRAM=terminal
MANPAGES1 = terminal.1

OBJS=\
terminal.o \

LIBS:=-lui -ldisplay

all: $(PROGRAM)

.PHONY: all install clean

install: all
	mkdir -p $(DESTDIR)$(BINDIR)
	install $(PROGRAM) $(DESTDIR)$(BINDIR)
	mkdir -p $(DESTDIR)$(MANDIR)/man1
	install $(MANPAGES1) $(DESTDIR)$(MANDIR)/man1

$(PROGRAM): $(OBJS)
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) $(OBJS) -o $@ $(LIBS)

%.o: %.c
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) -c $< -o $@

clean:
	rm -f $(PROGRAM) *.o
