SOFTWARE_MEANT_FOR_SORTIX=1
include ../build-aux/platform.mak
include ../build-aux/compiler.mak
include ../build-aux/dirs.mak

OPTLEVEL?=$(DEFAULT_OPTLEVEL)
CFLAGS?=$(OPTLEVEL)

CFLAGS:=$(CFLAGS) -Wall -Wextra

PROGRAM=display
MANPAGES1 = display.1
MANPAGES5 = displayrc.5

OBJS=\
connection.o \
display-code.o \
display.o \
server.o \
window.o \

LIBS:=-lui

all: $(PROGRAM)

.PHONY: all install clean

install: all
	mkdir -p $(DESTDIR)$(BINDIR)
	install $(PROGRAM) $(DESTDIR)$(BINDIR)
	mkdir -p $(DESTDIR)$(SYSCONFDIR)/default
	printf '#!sh\nexec terminal\n' > $(DESTDIR)$(SYSCONFDIR)/default/displayrc
	chmod +x $(DESTDIR)$(SYSCONFDIR)/default/displayrc
	mkdir -p $(DESTDIR)$(MANDIR)/man1
	install $(MANPAGES1) $(DESTDIR)$(MANDIR)/man1
	mkdir -p $(DESTDIR)$(MANDIR)/man5
	install $(MANPAGES5) $(DESTDIR)$(MANDIR)/man5

$(PROGRAM): $(OBJS)
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) $(OBJS) -o $@ $(LIBS)

display.o: arrow.inc

%.o: %.c
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) -c $< -o $@

%.inc: %.rgb
	carray -cs -o $@ $<

clean:
	rm -f $(PROGRAM) *.o *.inc
