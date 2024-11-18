#!/bin/sh
set -e

# Detect if the environment isn't set up properly.
if [ -z "$HOST" ]; then
  echo "$0: error: You need to set \$HOST" >&2
  exit 1
elif [ -z "$SORTIX_REPOSITORY_DIR" ]; then
  echo "$0: error: You need to set \$SORTIX_REPOSITORY_DIR" >&2
  exit 1
fi

if ! [ -d "$SORTIX_REPOSITORY_DIR" ]; then
  exit 0
fi
SORTIX_REPOSITORY_DIR="$SORTIX_REPOSITORY_DIR/$HOST"
if ! [ -d "$SORTIX_REPOSITORY_DIR" ]; then
  exit 0
fi

PACKAGES="$("$(dirname -- "$0")"/list-packages.sh PACKAGES)"

mkdir -p "$1"

for PACKAGE in $PACKAGES; do
  cp "$SORTIX_REPOSITORY_DIR/$PACKAGE.tix.tar.xz" "$1"
  cp "$SORTIX_REPOSITORY_DIR/$PACKAGE.version" "$1"
done

tix-repository --generation=3 metadata "$1"
