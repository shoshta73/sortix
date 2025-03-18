VERSION=1.1.0-dev
CHANNEL?=nightly
RELEASE?=$(VERSION)
RELEASE_AUTHORITATIVE?=https://pub.sortix.org/sortix
RELEASE_URL?=$(RELEASE_AUTHORITATIVE)/channel/$(CHANNEL)/$(VERSION)
BUILD_ID?=$(shell git rev-parse HEAD 2>/dev/null || echo $(RELEASE))
