VERSION=1.1dev
CHANNEL?=volatile
ifeq ($(CHANNEL),stable)
RELEASE?=$(VERSION)
endif
ifeq ($(CHANNEL),nightly)
RELEASE?=$(VERSION)
endif
RELEASE?=$(VERSION)-$(CHANNEL)
RELEASE_MASTER?=https://sortix.org/release
RELEASE_KEY=/etc/signify/sortix-$(VERSION).pub
RELEASE_SIG_URL?=$(RELEASE_MASTER)/$(RELEASE)/$(HOST_MACHINE).sh.sig
