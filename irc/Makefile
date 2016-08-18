include ../build-aux/platform.mak
include ../build-aux/compiler.mak
include ../build-aux/version.mak
include ../build-aux/dirs.mak

OPTLEVEL?=$(DEFAULT_OPTLEVEL)
CFLAGS?=$(OPTLEVEL)

CFLAGS += -Wall -Wextra
CPPFLAGS += -DVERSIONSTR=\"$(VERSION)\"

ifeq ($(HOST_IS_SORTIX),0)
  CPPFLAGS+=-D_GNU_SOURCE
endif

BINARY = irc
#MANPAGES1 = irc.1

OBJS=\
compat.o \
connection.o \
database.o \
irc.o \
scrollback.o \
string.o \
ui.o \

all: $(BINARY)

.PHONY: all install clean

$(OBJS): config.h

%.o: %.c
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BINARY): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(BINARY) $(LIBS)

install: all
	mkdir -p $(DESTDIR)$(BINDIR)
	install $(BINARY) $(DESTDIR)$(BINDIR)
	#mkdir -p $(DESTDIR)$(MANDIR)/man1
	#install $(MANPAGES1) $(DESTDIR)$(MANDIR)/man1

clean:
	rm -f $(BINARY)
	rm -f $(OBJS) *.o

TESTS=\
have-explicit_bzero \
have-reallocarray \
have-strlcat \
have-strlcpy \

include ../build-aux/config.mak
