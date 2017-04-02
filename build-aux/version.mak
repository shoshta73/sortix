VERSION=1.1dev
CHANNEL?=nightly
RELEASE?=$(VERSION)
RELEASE_MASTER?=https://sortix.org/release
RELEASE_KEY=/etc/signify/sortix-$(VERSION).pub
RELEASE_SIG_URL?=$(RELEASE_MASTER)/$(CHANNEL)/$(HOST_MACHINE).sh.sig
