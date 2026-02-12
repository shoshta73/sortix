include ../build-aux/platform.mak
include ../build-aux/compiler.mak
include ../build-aux/version.mak
include ../build-aux/dirs.mak

KBLAYOUTDIR?=$(DATAROOTDIR)/kblayout

KBLAYOUT_COMPILER?=kblayout-compiler

LAYOUTS=\
de \
dk \
fi \
fr \
uk \
us \

.PHONY: all install clean

all: $(LAYOUTS)

%: %.kblayout
	$(KBLAYOUT_COMPILER) --format=sortix-kblayout-1 --compression=none $< -o $@

install: all
	mkdir -m 755 -p $(DESTDIR)$(KBLAYOUTDIR)
	install -m 644 $(LAYOUTS) $(DESTDIR)$(KBLAYOUTDIR)

clean:
	rm -f $(LAYOUTS)
