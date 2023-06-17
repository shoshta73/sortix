SOFTWARE_MEANT_FOR_SORTIX=1
include ../build-aux/platform.mak
include ../build-aux/compiler.mak
include ../build-aux/dirs.mak

OPTLEVEL?=$(DEFAULT_OPTLEVEL)
CFLAGS?=$(OPTLEVEL)

CFLAGS:=$(CFLAGS) -Wall -Wextra

PROGRAM=nyan

OBJS=\
nyan.o \

LIBS:=-lui -ldisplay

all: $(PROGRAM)

.PHONY: all install clean

install: all
	mkdir -m 755 -p $(DESTDIR)$(BINDIR)
	install -m 755 $(PROGRAM) $(DESTDIR)$(BINDIR)

$(PROGRAM): $(OBJS)
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) $(OBJS) -o $@ $(LIBS)

%.o: %.c
	$(CC) -std=gnu11 $(CFLAGS) $(CPPFLAGS) -c $< -o $@

clean:
	rm -f $(PROGRAM) *.o
