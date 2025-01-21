SOFTWARE_MEANT_FOR_SORTIX=1
include build-aux/platform.mak
include build-aux/compiler.mak
include build-aux/version.mak

MODULES=\
libc \
libm \
libdisplay \
libmount \
libui \
bench \
carray \
checksum \
chkblayout \
chvideomode \
dhclient \
disked \
display \
dnsconfig \
editor \
ext \
games \
hostname \
ifconfig \
init \
kblayout \
kblayout-compiler \
login \
ping \
regress \
rw \
sf \
sh \
sysinstall \
terminal \
tix \
trianglix \
update-initrd \
utils \
kernel

ifndef SYSROOT
  SYSROOT:=$(shell pwd)/sysroot
endif

ifndef SYSROOT_OVERLAY
  SYSROOT_OVERLAY:=$(shell pwd)/sysroot-overlay
endif

SORTIX_BUILDS_DIR?=builds
SORTIX_MIRROR_DIR?=mirror
SORTIX_PORTS_DIR?=ports
SORTIX_RELEASE_DIR?=release
SORTIX_REPOSITORY_DIR?=repository
SORTIX_ISO_COMPRESSION?=xz

SORTIX_PORTS_MIRROR?=https://pub.sortix.org/mirror

# TODO: Update development(7).

SORTIX_RELEASE_ADDITIONAL?=
SORTIX_RELEASE_MANHTML?=yes
SORTIX_RELEASE_SOURCE?=yes

SORTIX_INCLUDE_SOURCE_GIT_REPO?=$(shell test -d .git && echo "file://`pwd`")
SORTIX_INCLUDE_SOURCE_GIT_REPO:=$(SORTIX_INCLUDE_SOURCE_GIT_REPO)
SORTIX_INCLUDE_SOURCE_GIT_ORIGIN?=https://sortix.org/sortix.git
SORTIX_INCLUDE_SOURCE_GIT_CLONE_OPTIONS?=--single-branch
SORTIX_INCLUDE_SOURCE_GIT_BRANCHES?=master
ifneq ($(and $(shell which git 2>/dev/null),$(SORTIX_INCLUDE_SOURCE_GIT_REPO)),)
  SORTIX_INCLUDE_SOURCE?=git
else
  SORTIX_INCLUDE_SOURCE?=yes
endif

include build-aux/dirs.mak

BUILD_NAME:=sortix-$(RELEASE)-$(MACHINE)

LIVE_INITRD:=$(SORTIX_BUILDS_DIR)/$(BUILD_NAME).live.tar
OVERLAY_INITRD:=$(SORTIX_BUILDS_DIR)/$(BUILD_NAME).overlay.tar
SRC_INITRD:=$(SORTIX_BUILDS_DIR)/$(BUILD_NAME).src.tar
SYSTEM_INITRD:=$(SORTIX_BUILDS_DIR)/$(BUILD_NAME).system.tix.tar

.PHONY: all
all: sysroot

.PHONY: sysmerge
sysmerge: sysroot
	sysmerge "$(SYSROOT)"

.PHONY: sysmerge-full
sysmerge-full: sysroot
	sysmerge --full "$(SYSROOT)"

.PHONY: sysmerge-full-now
sysmerge-full-now: sysroot
	sysmerge --full --now "$(SYSROOT)"

.PHONY: sysmerge-full-wait
sysmerge-full-wait: sysroot
	sysmerge --full --wait "$(SYSROOT)"

.PHONY: sysmerge-now
sysmerge-now: sysroot
	sysmerge --now "$(SYSROOT)"

.PHONY: sysmerge-wait
sysmerge-wait: sysroot
	sysmerge --wait "$(SYSROOT)"

.PHONY: clean-build-tools
clean-build-tools:
	$(MAKE) -C carray clean
	$(MAKE) -C kblayout-compiler clean
	$(MAKE) -C sf clean
	$(MAKE) -C tix clean

.PHONY: build-tools
build-tools:
	$(MAKE) -C carray
	$(MAKE) -C kblayout-compiler
	$(MAKE) -C sf
	$(MAKE) -C tix

.PHONY: install-build-tools
install-build-tools:
	$(MAKE) -C carray install
	$(MAKE) -C kblayout-compiler install
	$(MAKE) -C sf install
	$(MAKE) -C tix install

.PHONY: clean-cross-compiler
clean-cross-compiler:
	rm -rf ports/binutils/binutils.build
	rm -rf ports/gcc/gcc.build

ifeq ($(BUILD),$(TARGET))
CROSS_COMPILER_WITH_SYSROOT=--with-sysroot=/
else
CROSS_COMPILER_WITH_SYSROOT=--with-sysroot="$(SYSROOT)"
endif

.PHONY: install-cross-compiler
install-cross-compiler:
	$(MAKE) clean-sysroot
	$(MAKE) sysroot-base-headers HOST=$(TARGET) PREFIX=
	PATH="$(PREFIX)/sbin:$(PREFIX)/bin:$(PATH)" \
	$(MAKE) extract-ports PACKAGES='binutils gcc'
	rm -rf ports/binutils/binutils.build
	mkdir ports/binutils/binutils.build
	cd ports/binutils/binutils.build && \
	../binutils/configure \
	  --build="$(BUILD)" \
	  --target="$(TARGET)" \
	  --prefix="$(PREFIX)" \
	  $(CROSS_COMPILER_WITH_SYSROOT) \
	  --disable-werror \
	  --enable-default-execstack=no
	V=1 $(MAKE) -C ports/binutils/binutils.build
	V=1 $(MAKE) -C ports/binutils/binutils.build install
	rm -rf ports/gcc/gcc.build
	mkdir ports/gcc/gcc.build
	cd ports/gcc/gcc.build && \
	PATH="$(PREFIX)/bin:$(PATH)" \
	../gcc/configure \
	  --build="$(BUILD)" \
	  --target="$(TARGET)" \
	  --prefix="$(PREFIX)" \
	  $(CROSS_COMPILER_WITH_SYSROOT) \
	  --enable-languages=c,c++ \
	  --with-system-zlib
	PATH="$(PREFIX)/bin:$(PATH)" \
	$(MAKE) -C ports/gcc/gcc.build all-gcc all-target-libgcc
	PATH="$(PREFIX)/bin:$(PATH)" \
	$(MAKE) -C ports/gcc/gcc.build install-gcc install-target-libgcc
	rm -rf ports/gcc/gcc.build

.PHONY: clean-cross-toolchain
clean-cross-toolchain: clean-sysroot clean-build-tools clean-cross-compiler

.PHONY: install-cross-toolchain
install-cross-toolchain: install-build-tools
	$(MAKE) install-cross-compiler

.PHONY: install-cross-grub
install-cross-grub:
	$(MAKE) extract-ports PACKAGES=grub
	rm -rf ports/grub/grub.build
	mkdir ports/grub/grub.build
	cd ports/grub/grub.build && \
	../grub/configure --prefix="$(PREFIX)" --disable-werror --program-prefix= --with-platform=none
	$(MAKE) -C ports/grub/grub.build install
	rm -rf ports/grub/grub.build
	mkdir ports/grub/grub.build
	cd ports/grub/grub.build && \
	../grub/configure --prefix="$(PREFIX)" --disable-werror --program-prefix= --with-platform=pc --target=i686-sortix
	$(MAKE) -C ports/grub/grub.build install-grub-core
	rm -rf ports/grub/grub.build
	mkdir ports/grub/grub.build
	cd ports/grub/grub.build && \
	../grub/configure --prefix="$(PREFIX)" --disable-werror --program-prefix= --with-platform=efi --target=i686-sortix
	$(MAKE) -C ports/grub/grub.build install-grub-core
	rm -rf ports/grub/grub.build
	mkdir ports/grub/grub.build
	cd ports/grub/grub.build && \
	../grub/configure --prefix="$(PREFIX)" --disable-werror --program-prefix= --with-platform=efi --target=x86_64-sortix
	$(MAKE) -C ports/grub/grub.build install-grub-core
	rm -rf ports/grub/grub.build
	if [ ! -e "$(PREFIX)/share/grub/unicode.pf2" ]; then \
	  if [ -e /share/grub/unicode.pf2 ]; then \
	    cp /share/grub/unicode.pf2 "$(PREFIX)/share/grub/unicode.pf2"; \
	  elif [ -e /usr/share/grub/unicode.pf2 ]; then \
	    cp /usr/share/grub/unicode.pf2 "$(PREFIX)/share/grub/unicode.pf2"; \
	  fi; \
	fi

.PHONY: clean-cross-grub
clean-cross-grub:
	rm -rf ports/grub/grub.build

.PHONY: sysroot-fsh
sysroot-fsh:
	mkdir -p "$(SYSROOT)"
	mkdir -p "$(SYSROOT)/bin"
	mkdir -p "$(SYSROOT)/boot"
	mkdir -p "$(SYSROOT)/dev"
	mkdir -p "$(SYSROOT)/etc"
	mkdir -p "$(SYSROOT)/home"
	mkdir -p "$(SYSROOT)/include"
	mkdir -p "$(SYSROOT)/lib"
	mkdir -p "$(SYSROOT)/libexec"
	mkdir -p "$(SYSROOT)/mnt"
	mkdir -p "$(SYSROOT)/sbin"
	mkdir -p "$(SYSROOT)/share"
	mkdir -p "$(SYSROOT)/src"
	mkdir -p "$(SYSROOT)/tix"
	mkdir -p "$(SYSROOT)/tix/tixinfo"
	mkdir -p "$(SYSROOT)/tix/manifest"
	mkdir -p "$(SYSROOT)/tmp" -m 1777
	mkdir -p "$(SYSROOT)/var"
	mkdir -p "$(SYSROOT)/var/cache"
	mkdir -p "$(SYSROOT)/var/empty" -m 555
	mkdir -p "$(SYSROOT)/var/log"
	mkdir -p "$(SYSROOT)/var/run"
	ln -sfT . "$(SYSROOT)/usr"

.PHONY: sysroot-base-headers
sysroot-base-headers: sysroot-fsh
	export SYSROOT="$(SYSROOT)" && \
	(for D in libc libm kernel; do ($(MAKE) -C $$D install-headers DESTDIR="$(SYSROOT)") || exit $$?; done)

.PHONY: sysroot-system
sysroot-system: sysroot-fsh sysroot-base-headers
	rm -f "$(SYSROOT)/tix/manifest/system"
	echo / >> "$(SYSROOT)/tix/manifest/system"
	echo /bin >> "$(SYSROOT)/tix/manifest/system"
	echo /boot >> "$(SYSROOT)/tix/manifest/system"
	echo /dev >> "$(SYSROOT)/tix/manifest/system"
	echo /etc >> "$(SYSROOT)/tix/manifest/system"
	echo /home >> "$(SYSROOT)/tix/manifest/system"
	echo /include >> "$(SYSROOT)/tix/manifest/system"
	echo /lib >> "$(SYSROOT)/tix/manifest/system"
	echo /libexec >> "$(SYSROOT)/tix/manifest/system"
	echo /mnt >> "$(SYSROOT)/tix/manifest/system"
	echo /sbin >> "$(SYSROOT)/tix/manifest/system"
	echo /share >> "$(SYSROOT)/tix/manifest/system"
	echo /src >> "$(SYSROOT)/tix/manifest/system"
	echo /tmp >> "$(SYSROOT)/tix/manifest/system"
	echo /usr >> "$(SYSROOT)/tix/manifest/system"
	echo /var >> "$(SYSROOT)/tix/manifest/system"
	echo /var/cache >> "$(SYSROOT)/tix/manifest/system"
	echo /var/empty >> "$(SYSROOT)/tix/manifest/system"
	echo /var/log >> "$(SYSROOT)/tix/manifest/system"
	echo /var/run >> "$(SYSROOT)/tix/manifest/system"
	(echo 'NAME="Sortix"' && \
	 echo 'VERSION="$(VERSION)"' && \
	 echo 'ID=sortix' && \
	 echo 'VERSION_ID="$(VERSION)"' && \
	 echo 'PRETTY_NAME="Sortix $(VERSION)"' && \
	 echo 'ARCHITECTURE="$(HOST_MACHINE)"' && \
	 echo 'SORTIX_ABI=2.2' && \
	 true) > "$(SYSROOT)/lib/sortix-release"
	echo /lib/sortix-release >> "$(SYSROOT)/tix/manifest/system"
	ln -sf sortix-release "$(SYSROOT)/lib/os-release"
	echo /lib/os-release >> "$(SYSROOT)/tix/manifest/system"
	# TODO: After releasing Sortix 1.1, delete these machine and sortix-release
	#       compatibility files needed to sysmerge from Sortix 1.0.
	echo "$(HOST_MACHINE)" > "$(SYSROOT)/lib/machine"
	echo /lib/machine >> "$(SYSROOT)/tix/manifest/system"
	ln -sf ../lib/machine "$(SYSROOT)/etc/machine"
	echo /etc/machine >> "$(SYSROOT)/tix/manifest/system"
	ln -sf ../lib/sortix-release "$(SYSROOT)/etc/sortix-release"
	echo /etc/sortix-release >> "$(SYSROOT)/tix/manifest/system"
	find share | sed -e 's,^,/,' >> "$(SYSROOT)/tix/manifest/system"
	cp -RT share "$(SYSROOT)/share"
	export SYSROOT="$(SYSROOT)" && \
	(for D in $(MODULES); \
	  do ($(MAKE) -C $$D && \
	      rm -rf "$(SYSROOT).destdir" && \
	      mkdir -p "$(SYSROOT).destdir" && \
	      $(MAKE) -C $$D install DESTDIR="$(SYSROOT).destdir" && \
	      (cd "$(SYSROOT).destdir" && find .) | sed -e 's/\.//' -e 's/^$$/\//' | \
	      grep -E '^.+$$' >> "$(SYSROOT)/tix/manifest/system" && \
	      cp -RT "$(SYSROOT).destdir" "$(SYSROOT)" && \
	      rm -rf "$(SYSROOT).destdir") \
	  || exit $$?; done)
	LC_ALL=C sort -u "$(SYSROOT)/tix/manifest/system" > "$(SYSROOT)/tix/manifest/system.new"
	mv "$(SYSROOT)/tix/manifest/system.new" "$(SYSROOT)/tix/manifest/system"
	printf 'TIX_VERSION=3\nNAME=system\nPLATFORM=$(HOST)\nPREFIX=\nSYSTEM=true\n' > "$(SYSROOT)/tix/tixinfo/system"

.PHONY: sysroot-source
sysroot-source: sysroot-fsh
ifeq ($(SORTIX_INCLUDE_SOURCE),git)
	rm -rf "$(SYSROOT)/src"
	git clone --no-hardlinks $(SORTIX_INCLUDE_SOURCE_GIT_CLONE_OPTIONS) -- "$(SORTIX_INCLUDE_SOURCE_GIT_REPO)" "$(SYSROOT)/src"
	(cd "$(SYSROOT)/src" && git config remote.origin.fetch '+refs/heads/*:refs/remotes/origin/*')
	-cd "$(SYSROOT)/src" && for BRANCH in $(SORTIX_INCLUDE_SOURCE_GIT_BRANCHES); do \
	  git fetch origin $$BRANCH:refs/remotes/origin/$$BRANCH && \
	  (git branch -f $$BRANCH origin/$$BRANCH || true) ; \
	done
ifneq ($(SORTIX_INCLUDE_SOURCE_GIT_ORIGIN),)
	cd "$(SYSROOT)/src" && git remote set-url origin $(SORTIX_INCLUDE_SOURCE_GIT_ORIGIN)
else
	-cd "$(SYSROOT)/src" && git remote rm origin
endif
else ifneq ($(SORTIX_INCLUDE_SOURCE),no)
	cp .gitignore -t "$(SYSROOT)/src"
	cp LICENSE -t "$(SYSROOT)/src"
	cp Makefile -t "$(SYSROOT)/src"
	cp README -t "$(SYSROOT)/src"
	cp -RT build-aux "$(SYSROOT)/src/build-aux"
	cp -RT share "$(SYSROOT)/src/share"
	cp -RT ports "$(SYSROOT)/src/ports"
	(for D in $(MODULES); do cp -R $$D -t "$(SYSROOT)/src" || exit $$?; done)
	$(MAKE) -C "$(SYSROOT)/src" distclean
endif
	(cd "$(SYSROOT)" && find .) | sed 's/\.//' | \
	grep -E '^/src(/.*)?$$' | \
	LC_ALL=C sort > "$(SYSROOT)/tix/manifest/src"

.PHONY: available-ports
available-ports:
	@for port in $$(tix-list-packages --ports="$(SORTIX_PORTS_DIR)" $${PACKAGES-all!!}); do \
	  build-aux/upgrade-port.sh ports/$$port/$$port.port available; \
	done

.PHONY: upgrade-ports
upgrade-ports:
	@for port in $$(tix-list-packages --ports="$(SORTIX_PORTS_DIR)" $${PACKAGES-all!!}); do \
	  build-aux/upgrade-port.sh ports/$$port/$$port.port upgrade; \
	done

.PHONY: mirror
mirror:
	@SORTIX_MIRROR_DIR="$(SORTIX_MIRROR_DIR)" \
	 SORTIX_PORTS_DIR="$(SORTIX_PORTS_DIR)" \
	 SORTIX_REPOSITORY_DIR="$(SORTIX_REPOSITORY_DIR)" \
	 SORTIX_PORTS_MIRROR="$(SORTIX_PORTS_MIRROR)" \
	 SYSROOT="$(SYSROOT)" \
	 BUILD="$(BUILD)" \
	 HOST="$(HOST)" \
	 MAKE="$(MAKE)" \
	 MAKEFLAGS="$(MAKEFLAGS)" \
	 build-aux/build-ports.sh download

.PHONY: extract-ports
extract-ports:
	@SORTIX_MIRROR_DIR="$(SORTIX_MIRROR_DIR)" \
	 SORTIX_PORTS_DIR="$(SORTIX_PORTS_DIR)" \
	 SORTIX_REPOSITORY_DIR="$(SORTIX_REPOSITORY_DIR)" \
	 SORTIX_PORTS_MIRROR="$(SORTIX_PORTS_MIRROR)" \
	 SYSROOT="$(SYSROOT)" \
	 BUILD="$(BUILD)" \
	 HOST="$(HOST)" \
	 MAKE="$(MAKE)" \
	 MAKEFLAGS="$(MAKEFLAGS)" \
	 build-aux/build-ports.sh extract

.PHONY: sysroot-ports
sysroot-ports: sysroot-fsh sysroot-base-headers sysroot-system sysroot-source
	@SORTIX_MIRROR_DIR="$(SORTIX_MIRROR_DIR)" \
	 SORTIX_PORTS_DIR="$(SORTIX_PORTS_DIR)" \
	 SORTIX_REPOSITORY_DIR="$(SORTIX_REPOSITORY_DIR)" \
	 SORTIX_PORTS_MIRROR="$(SORTIX_PORTS_MIRROR)" \
	 SYSROOT="$(SYSROOT)" \
	 BUILD="$(BUILD)" \
	 HOST="$(HOST)" \
	 MAKE="$(MAKE)" \
	 MAKEFLAGS="$(MAKEFLAGS)" \
	 build-aux/build-ports.sh build

.PHONY: sysroot
sysroot: sysroot-system sysroot-source sysroot-ports

$(SORTIX_REPOSITORY_DIR):
	mkdir -p $@

$(SORTIX_REPOSITORY_DIR)/$(HOST): $(SORTIX_REPOSITORY_DIR)
	mkdir -p $@

.PHONY: clean-core
clean-core:
	(for D in $(MODULES); do $(MAKE) clean -C $$D || exit $$?; done)

.PHONY: clean-mirror
clean-mirror:
	rm -rf "$(SORTIX_MIRROR_DIR)"

.PHONY: clean-ports
clean-ports:
	@SORTIX_PORTS_DIR="$(SORTIX_PORTS_DIR)" \
	 MAKE="$(MAKE)" \
	 MAKEFLAGS="$(MAKEFLAGS)" \
	 build-aux/build-ports.sh clean

.PHONY: clean-builds
clean-builds:
	rm -rf "$(SORTIX_BUILDS_DIR)"
	rm -f sortix.iso

.PHONY: clean-release
clean-release:
	rm -rf "$(SORTIX_RELEASE_DIR)"

.PHONY: clean-repository
clean-repository:
	rm -rf "$(SORTIX_REPOSITORY_DIR)"

.PHONY: clean-sysroot
clean-sysroot:
	rm -rf "$(SYSROOT)"
	rm -rf "$(SYSROOT)".destdir

.PHONY: clean
clean: clean-core clean-ports

.PHONY: distclean-ports
distclean-ports:
	@SORTIX_PORTS_DIR="$(SORTIX_PORTS_DIR)" \
	 MAKE="$(MAKE)" \
	 MAKEFLAGS="$(MAKEFLAGS)" \
	 build-aux/build-ports.sh distclean

.PHONY: mostlyclean
mostlyclean: clean-core distclean-ports clean-builds clean-release clean-sysroot clean-cross-compiler

.PHONY: distclean
distclean: clean-core distclean-ports clean-builds clean-release clean-mirror clean-repository clean-sysroot clean-cross-compiler

.PHONY: most-things
most-things: sysroot iso

.PHONY: everything
everything: most-things

# Targets that build multiple architectures.

.PHONY: sysroot-base-headers-all-archs
sysroot-base-headers-all-archs:
	$(MAKE) clean clean-sysroot
	$(MAKE) sysroot-base-headers HOST=i686-sortix
	$(MAKE) clean clean-sysroot
	$(MAKE) sysroot-base-headers HOST=x86_64-sortix

.PHONY: install-cross-compiler-all-archs
install-cross-compiler-all-archs:
	$(MAKE) clean-cross-compiler
	$(MAKE) install-cross-compiler TARGET=i686-sortix
	$(MAKE) clean-cross-compiler
	$(MAKE) install-cross-compiler TARGET=x86_64-sortix

.PHONY: install-cross-toolchain-all-archs
install-cross-toolchain-all-archs: install-build-tools
	$(MAKE) install-cross-compiler-all-archs

.PHONY: all-archs
all-archs:
	$(MAKE) clean clean-sysroot
	$(MAKE) all HOST=i686-sortix
	$(MAKE) clean clean-sysroot
	$(MAKE) all HOST=x86_64-sortix

.PHONY: most-things-all-archs
most-things-all-archs:
	$(MAKE) clean clean-sysroot
	$(MAKE) most-things HOST=i686-sortix
	$(MAKE) clean clean-sysroot
	$(MAKE) most-things HOST=x86_64-sortix

.PHONY: everything-all-archs
everything-all-archs:
	$(MAKE) clean clean-sysroot
	$(MAKE) everything HOST=i686-sortix
	$(MAKE) clean clean-sysroot
	$(MAKE) everything HOST=x86_64-sortix

.PHONY: release-all-archs
release-all-archs:
	$(MAKE) clean clean-sysroot
	$(MAKE) release-arch HOST=i686-sortix
	$(MAKE) clean clean-sysroot
	$(MAKE) release HOST=x86_64-sortix

# Initial ramdisk

$(LIVE_INITRD): sysroot
	mkdir -p `dirname $(LIVE_INITRD)`
	rm -rf $(LIVE_INITRD).d
	mkdir -p $(LIVE_INITRD).d
	mkdir -p $(LIVE_INITRD).d/etc
	mkdir -p $(LIVE_INITRD).d/etc/init
	echo require single-user exit-code > $(LIVE_INITRD).d/etc/init/default
	echo "root::0:0:root:/root:sh" > $(LIVE_INITRD).d/etc/passwd
	echo "include /etc/default/passwd.d/*" >> $(LIVE_INITRD).d/etc/passwd
	echo "root::0:root" > $(LIVE_INITRD).d/etc/group
	echo "include /etc/default/group.d/*" >> $(LIVE_INITRD).d/etc/group
	mkdir -p $(LIVE_INITRD).d/root -m 700
	if [ -e "$(SYSROOT)/etc/skel" ]; then cp -RT "$(SYSROOT)/etc/skel" $(LIVE_INITRD).d/root; fi
	(echo "You can view the documentation for new users by typing:" && \
	 echo && \
	 echo "  man user-guide" && \
	 echo && \
	 echo "You can view the installation instructions by typing:" && \
	 echo && \
	 echo "  man installation") > $(LIVE_INITRD).d/root/welcome
	tix-collection $(LIVE_INITRD).d create --platform=$(HOST) --prefix= --generation=3
	LC_ALL=C ls -A $(LIVE_INITRD).d | \
	tar -cf $(LIVE_INITRD) -C $(LIVE_INITRD).d --numeric-owner --owner=0 --group=0 -T -
	rm -rf $(LIVE_INITRD).d

.PHONY: $(OVERLAY_INITRD)
$(OVERLAY_INITRD): sysroot
	mkdir -p `dirname $(OVERLAY_INITRD)`
	test ! -d "$(SYSROOT_OVERLAY)" || \
	LC_ALL=C ls -A "$(SYSROOT_OVERLAY)" | \
	tar -cf $(OVERLAY_INITRD) -C "$(SYSROOT_OVERLAY)" --numeric-owner --owner=0 --group=0 -T -

$(SRC_INITRD): sysroot
	mkdir -p `dirname $(SRC_INITRD)`
	sed -E 's,^/,,' "$(SYSROOT)/tix/manifest/src" | \
	tar -cf $(SRC_INITRD) -C "$(SYSROOT)" --numeric-owner --owner=0 --group=0 --no-recursion -T - tix tix/manifest tix/manifest/src

$(SYSTEM_INITRD): sysroot
	mkdir -p `dirname $(SYSTEM_INITRD)`
	sed -E 's,^/,,' "$(SYSROOT)/tix/manifest/system" | \
	tar -cf $(SYSTEM_INITRD) -C "$(SYSROOT)" --numeric-owner --owner=0 --group=0 --no-recursion -T - tix tix/manifest tix/manifest/system tix/tixinfo tix/tixinfo/system

# Packaging

$(SORTIX_BUILDS_DIR):
	mkdir -p $(SORTIX_BUILDS_DIR)

# Bootable images

$(SORTIX_BUILDS_DIR)/$(BUILD_NAME).iso: sysroot $(LIVE_INITRD) $(OVERLAY_INITRD) $(SRC_INITRD) $(SYSTEM_INITRD) $(SORTIX_BUILDS_DIR)
	rm -rf $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso
	mkdir -p $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso
	mkdir -p $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso/boot
	mkdir -p $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso/repository
	SORTIX_PORTS_DIR="$(SORTIX_PORTS_DIR)" \
	SORTIX_REPOSITORY_DIR="$(SORTIX_REPOSITORY_DIR)" \
	SYSROOT="$(SYSROOT)" \
	HOST="$(HOST)" \
	build-aux/iso-repository.sh $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso/repository
ifeq ($(SORTIX_ISO_COMPRESSION),xz)
	xz -c "$(SYSROOT)/boot/sortix.bin" > $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso/boot/sortix.bin.xz
	xz -c $(LIVE_INITRD) > $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso/boot/live.tar.xz
	test ! -e "$(OVERLAY_INITRD)" || \
	xz -c $(OVERLAY_INITRD) > $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso/boot/overlay.tar.xz
	xz -c $(SRC_INITRD) > $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso/boot/src.tar.xz
	xz -c $(SYSTEM_INITRD) > $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso/repository/system.tix.tar.xz
	build-aux/iso-grub-cfg.sh --platform $(HOST) --version $(VERSION) $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso
	grub-mkrescue --compress=xz -o $(SORTIX_BUILDS_DIR)/$(BUILD_NAME).iso $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso
else ifeq ($(SORTIX_ISO_COMPRESSION),gzip)
	gzip -c "$(SYSROOT)/boot/sortix.bin" > $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso/boot/sortix.bin.gz
	gzip -c $(LIVE_INITRD) > $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso/boot/live.tar.gz
	test ! -e "$(OVERLAY_INITRD)" || \
	gzip -c $(OVERLAY_INITRD) > $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso/boot/overlay.tar.gz
	gzip -c $(SRC_INITRD) > $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso/boot/src.tar.gz
	gzip -c $(SYSTEM_INITRD) > $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso/repository/system.tix.tar.gz
	build-aux/iso-grub-cfg.sh --platform $(HOST) --version $(VERSION) $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso
	grub-mkrescue --compress=gz -o $(SORTIX_BUILDS_DIR)/$(BUILD_NAME).iso $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso
else # none
	cp "$(SYSROOT)/boot/sortix.bin" $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso/boot/sortix.bin
	cp $(LIVE_INITRD) $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso/boot/live.tar
	test ! -e "$(OVERLAY_INITRD)" || \
	cp $(OVERLAY_INITRD) $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso/boot/overlay.tar
	cp $(SRC_INITRD) $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso/boot/src.tar
	cp $(SYSTEM_INITRD) $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso/repository/system.tix.tar
	build-aux/iso-grub-cfg.sh --platform $(HOST) --version $(VERSION) $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso
	grub-mkrescue -o $(SORTIX_BUILDS_DIR)/$(BUILD_NAME).iso $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso
endif
	rm -rf $(SORTIX_BUILDS_DIR)/$(BUILD_NAME)-iso

.PHONY: iso
iso: $(SORTIX_BUILDS_DIR)/$(BUILD_NAME).iso

sortix.iso: $(SORTIX_BUILDS_DIR)/$(BUILD_NAME).iso
	cp $< $@

# Release

$(SORTIX_RELEASE_DIR)/$(RELEASE):
	mkdir -p $@

$(SORTIX_RELEASE_DIR)/$(RELEASE)/builds: $(SORTIX_RELEASE_DIR)/$(RELEASE)
	mkdir -p $@

$(SORTIX_RELEASE_DIR)/$(RELEASE)/builds/$(BUILD_NAME).iso: $(SORTIX_BUILDS_DIR)/$(BUILD_NAME).iso $(SORTIX_RELEASE_DIR)/$(RELEASE)/builds
	cp $< $@

ifneq ($(MACHINE),)
$(SORTIX_RELEASE_DIR)/$(RELEASE)/$(MACHINE): $(SORTIX_RELEASE_DIR)/$(RELEASE)
	mkdir -p $@
endif

$(SORTIX_RELEASE_DIR)/$(RELEASE)/$(MACHINE)/boot: $(SORTIX_RELEASE_DIR)/$(RELEASE)/$(MACHINE)
	mkdir -p $@

$(SORTIX_RELEASE_DIR)/$(RELEASE)/$(MACHINE)/boot/sortix.bin.xz: sysroot $(SORTIX_RELEASE_DIR)/$(RELEASE)/$(MACHINE)/boot
	xz -c "$(SYSROOT)/boot/sortix.bin" > $@

$(SORTIX_RELEASE_DIR)/$(RELEASE)/$(MACHINE)/boot/live.tar.xz: $(LIVE_INITRD) $(SORTIX_RELEASE_DIR)/$(RELEASE)/$(MACHINE)/boot
	xz -c $< > $@

$(SORTIX_RELEASE_DIR)/$(RELEASE)/$(MACHINE)/boot/overlay.tar.xz: $(OVERLAY_INITRD) $(SORTIX_RELEASE_DIR)/$(RELEASE)/$(MACHINE)/boot
	test ! -e $< || xz -c $< > $@

$(SORTIX_RELEASE_DIR)/$(RELEASE)/$(MACHINE)/boot/src.tar.xz: $(SRC_INITRD) $(SORTIX_RELEASE_DIR)/$(RELEASE)/$(MACHINE)/boot
	xz -c $< > $@

.PHONY: release-boot
release-boot: \
  $(SORTIX_RELEASE_DIR)/$(RELEASE)/$(MACHINE)/boot/sortix.bin.xz \
  $(SORTIX_RELEASE_DIR)/$(RELEASE)/$(MACHINE)/boot/live.tar.xz \
  $(SORTIX_RELEASE_DIR)/$(RELEASE)/$(MACHINE)/boot/overlay.tar.xz \
  $(SORTIX_RELEASE_DIR)/$(RELEASE)/$(MACHINE)/boot/src.tar.xz \

.PHONY: release-iso
release-iso: $(SORTIX_RELEASE_DIR)/$(RELEASE)/builds/$(BUILD_NAME).iso

.PHONY: release-builds
release-builds: release-boot release-iso

$(SORTIX_RELEASE_DIR)/$(RELEASE)/scripts: $(SORTIX_RELEASE_DIR)/$(RELEASE)
	mkdir -p $@

$(SORTIX_RELEASE_DIR)/$(RELEASE)/scripts/tix-iso-add: tix/tix-iso-add $(SORTIX_RELEASE_DIR)/$(RELEASE)/scripts
	cp $< $@

$(SORTIX_RELEASE_DIR)/$(RELEASE)/scripts/tix-iso-bootconfig: tix/tix-iso-bootconfig $(SORTIX_RELEASE_DIR)/$(RELEASE)/scripts
	cp $< $@

$(SORTIX_RELEASE_DIR)/$(RELEASE)/scripts/tix-iso-liveconfig: tix/tix-iso-liveconfig $(SORTIX_RELEASE_DIR)/$(RELEASE)/scripts
	cp $< $@

$(SORTIX_RELEASE_DIR)/$(RELEASE)/man:
	mkdir -p $@

$(SORTIX_RELEASE_DIR)/$(RELEASE)/man/ports.list: sysroot $(SORTIX_RELEASE_DIR)/$(RELEASE)/man
	for section in 1 2 3 4 5 6 7 8 9; do mkdir -p $(SORTIX_RELEASE_DIR)/$(RELEASE)/man/man$$section; done
	for port in `LC_ALL=C ls "$(SYSROOT)/tix/tixinfo"`; do \
	  for manpage in `grep -E "^/share/man/man[1-9]/.*\.[1-9]$$" "$(SYSROOT)/tix/manifest/$$port" | \
	                  LC_ALL=C sort | \
	                  tee $(SORTIX_RELEASE_DIR)/$(RELEASE)/man/$$port.list | \
	                  grep -Eo 'man[1-9]/[^/]*\.[0-9]$$'`; do \
	    cp -f "$(SYSROOT)/share/man/$$manpage" $(SORTIX_RELEASE_DIR)/$(RELEASE)/man/$$manpage && \
	    chmod 644 $(SORTIX_RELEASE_DIR)/$(RELEASE)/man/$$manpage; \
	  done; \
	done
	LC_ALL=C ls "$(SYSROOT)/tix/tixinfo" > $(SORTIX_RELEASE_DIR)/$(RELEASE)/man/ports.list

$(SORTIX_RELEASE_DIR)/$(RELEASE)/repository/$(HOST):
	mkdir -p $@

.PHONY: release-repository
release-repository: sysroot $(SYSTEM_INITRD) $(SORTIX_RELEASE_DIR)/$(RELEASE)/repository/$(HOST)
	xz -c $(SYSTEM_INITRD) > $(SORTIX_RELEASE_DIR)/$(RELEASE)/repository/$(HOST)/system.tix.tar.xz
	for port in `LC_ALL=C ls "$(SYSROOT)/tix/tixinfo" | (grep -Ev '^system$$' || true)`; do \
	  cp $(SORTIX_REPOSITORY_DIR)/$(HOST)/$$port.tix.tar.xz $(SORTIX_RELEASE_DIR)/$(RELEASE)/repository/$(HOST) && \
	  cp $(SORTIX_REPOSITORY_DIR)/$(HOST)/$$port.version $(SORTIX_RELEASE_DIR)/$(RELEASE)/repository/$(HOST); \
	done
	tix-repository --generation=3 metadata $(SORTIX_RELEASE_DIR)/$(RELEASE)/repository/$(HOST)

.PHONY: release-scripts
release-scripts: \
  $(SORTIX_RELEASE_DIR)/$(RELEASE)/scripts/tix-iso-add \
  $(SORTIX_RELEASE_DIR)/$(RELEASE)/scripts/tix-iso-bootconfig \
  $(SORTIX_RELEASE_DIR)/$(RELEASE)/scripts/tix-iso-liveconfig \

$(SORTIX_RELEASE_DIR)/$(RELEASE)/README: README $(SORTIX_RELEASE_DIR)/$(RELEASE)
	cp $< $@

.PHONY: release-man
release-man: $(SORTIX_RELEASE_DIR)/$(RELEASE)/man/ports.list

.PHONY: release-man-html
release-man-html: release-man
ifeq ($(SORTIX_RELEASE_MANHTML),yes)
	RELEASE="$(RELEASE)" build-aux/manhtml.sh $(SORTIX_RELEASE_DIR)/$(RELEASE)/man
endif

.PHONY: release-readme
release-readme: $(SORTIX_RELEASE_DIR)/$(RELEASE)/README

.PHONY: release-additional
release-additional: $(SORTIX_RELEASE_DIR)/$(RELEASE)
ifneq ($(SORTIX_RELEASE_ADDITIONAL),)
	cp -RT $(SORTIX_RELEASE_ADDITIONAL) $(SORTIX_RELEASE_DIR)/$(RELEASE)
endif

.PHONY: release-arch
release-arch: release-builds release-repository

# Depend on sysroot-ports because sysroot-ports would race with mirror and the
# build must be offline if the local mirror is already populated.
.PHONY: release-source
ifeq ($(SORTIX_RELEASE_SOURCE),yes)
release-source: sysroot-source sysroot-ports
	rm -rf $(SORTIX_RELEASE_DIR)/$(RELEASE)/source
	mkdir -p $(SORTIX_RELEASE_DIR)/$(RELEASE)/source
	cp -RT "$(SYSROOT)/src" $(SORTIX_RELEASE_DIR)/$(RELEASE)/source/sortix-$(RELEASE)
	rm -rf $(SORTIX_RELEASE_DIR)/$(RELEASE)/source/sortix-$(RELEASE)/.git
	cd $(SORTIX_RELEASE_DIR)/$(RELEASE)/source && tar -f sortix-$(RELEASE).tar.xz -cJ sortix-$(RELEASE)
	mv $(SORTIX_RELEASE_DIR)/$(RELEASE)/source/sortix-$(RELEASE) $(SORTIX_RELEASE_DIR)/$(RELEASE)/source/sortix-$(RELEASE)-full
	SORTIX_PORTS_MIRROR=`realpath $(SORTIX_MIRROR_DIR)` $(MAKE) -C $(SORTIX_RELEASE_DIR)/$(RELEASE)/source/sortix-$(RELEASE)-full mirror
	cd $(SORTIX_RELEASE_DIR)/$(RELEASE)/source && tar -f sortix-$(RELEASE)-full.tar.xz -cJ sortix-$(RELEASE)-full
	rm -rf $(SORTIX_RELEASE_DIR)/$(RELEASE)/source/sortix-$(RELEASE)-full
else
release-source:
endif

.PHONY: release-shared
release-shared: release-man release-man-html release-readme release-scripts release-additional release-source

.PHONY: release
release: release-arch release-shared
	cd $(SORTIX_RELEASE_DIR)/$(RELEASE) && \
	find . -type f '!' -name sha256sum '!' -name '*.html' -exec sha256sum '{}' ';' | \
	sed -E 's,^([^ ]*  )\./,\1,' | \
	LC_ALL=C sort -k 2 > sha256sum

# Presubmit checks

presubmit:
	$(MAKE) verify-ports
	$(MAKE) verify-coding-style
	$(MAKE) verify-manual
	$(MAKE) verify-build-tools
# TODO: The gcc port doesn't ship with cross-compilers out of the box.
ifeq ($(BUILD_IS_SORTIX),1)
	$(MAKE) verify-build
else
	$(MAKE) verify-build HOST=i686-sortix
	$(MAKE) verify-build HOST=x86_64-sortix
endif
	$(MAKE) verify-sysroot-source
	$(MAKE) verify-headers
	@echo ok

verify-ports:
	build-aux/verify-ports.sh

verify-coding-style:
	build-aux/verify-coding-style.sh

verify-manual:
	build-aux/verify-manual.sh

verify-build-tools:
	$(MAKE) clean-build-tools
	CFLAGS='-O2 -g -Werror -Werror=strict-prototypes' CXXFLAGS='-O2 -g -Werror' $(MAKE) build-tools

verify-sysroot-source:
	$(MAKE) clean-sysroot
	$(MAKE) sysroot-source SORTIX_INCLUDE_SOURCE=yes
	git ls-files | sort > "$(SYSROOT)/src.want"
	(cd "$(SYSROOT)/src" && find '!' -type d | sort | sed -E 's,^\./,,') > "$(SYSROOT)/src.got"
	diff -u "$(SYSROOT)/src.want" "$(SYSROOT)/src.got"
	rm -f "$(SYSROOT)/src.want" "$(SYSROOT)/src.got"

verify-build:
	$(MAKE) mostlyclean
	CFLAGS='-O2 -g -Werror -Werror=strict-prototypes' CXXFLAGS='-O2 -g -Werror' $(MAKE) PACKAGES=''

verify-headers:
# TODO: The gcc port doesn't ship with cross-compilers out of the box.
ifeq ($(BUILD_IS_SORTIX),1)
	build-aux/verify-headers.sh $(HOST) # Inherit jobserver: $(MAKE)
else
	build-aux/verify-headers.sh # Inherit jobserver: $(MAKE)
endif

# Virtualization
.PHONY: run-virtualbox
run-virtualbox: sortix.iso
	virtualbox --startvm sortix

.PHONY: run-virtualbox-debug
run-virtualbox-debug: sortix.iso
	virtualbox --debug --start-running --startvm sortix

# Statistics
.PHONY: linecount
linecount:
	wc -l `git ls-files | grep -Ev '^libm/man/.*$$'` | LC_ALL=C sort
