#!/bin/sh
set -e

make_dir_path_absolute() {
  (cd "$1" && pwd)
}

has_command() {
  which "$1" > /dev/null
}

# Determine what's supposed to happen.
if [ $# = 0 ]; then
  echo "$0: usage: $0 <operation>" >&2
  exit 1
fi
OPERATION="$1"
CLEAN=false
unset CACHE_PACKAGE
unset DISTCLEAN
unset END
unset RANDOMIZE
unset START
case "$OPERATION" in
distclean) CLEAN=true; DISTCLEAN=--distclean ;;
clean) CLEAN=true; START=clean; END=clean ;;
download) END=download ;;
extract) END=extract ;;
build) CACHE_PACKAGE=--cache-package; RANDOMIZE=--randomize ;;
*) echo "$0: error: Invalid operation: $OPERATION" >&2
   exit 1
esac

# TODO: After releasing Sortix 1.1, remove support for building with Sortix 1.0
#       that doesn't have sort -R.
if [ -n "$RANDOMIZE" ]; then
  if ! true | sort -R > /dev/null 2>&1; then
    RANDOMIZE=
  fi
fi

# Detect if the environment isn't set up properly.
if ! $clean && [ -z "$HOST" ]; then
  echo "$0: error: You need to set \$HOST" >&2
  exit 1
elif ! $clean && [ -z "$SYSROOT" ]; then
  echo "$0: error: You need to set \$SYSROOT" >&2
  exit 1
elif [ -z "$SORTIX_PORTS_DIR" ]; then
  echo "$0: error: You need to set \$SORTIX_PORTS_DIR" >&2
  exit 1
elif ! $clean && [ -z "$SORTIX_MIRROR_DIR" ]; then
  echo "$0: error: You need to set \$SORTIX_MIRROR_DIR" >&2
  exit 1
elif ! $clean && [ -z "$SORTIX_REPOSITORY_DIR" ]; then
  echo "$0: error: You need to set \$SORTIX_REPOSITORY_DIR" >&2
  exit 1
elif ! [ -d "$SORTIX_PORTS_DIR" ]; then
  echo "Warning: No ports directory found, third party software will not be built"
  exit 0
elif ! has_command tix-metabuild; then
  if $clean; then
    echo "$0: warning: You need to have Tix installed Tix to clean ports." >&2
    exit 0
  else
    echo "$0: error: You need to have Tix installed Tix to compile ports." >&2
    exit 1
  fi
fi

if ! $CLEAN; then
  # Create the mirror directory for downloaded archives.
  mkdir -p "$SORTIX_MIRROR_DIR"

  # Add the platform triplet to the binary repository path.
  if [ "$OPERATION" = build ]; then
    SORTIX_REPOSITORY_DIR="$SORTIX_REPOSITORY_DIR/$HOST"
    mkdir -p "$SORTIX_REPOSITORY_DIR"
  fi

  # Create the system root if absent.
  if [ "$OPERATION" = build ]; then
    mkdir -p "$SYSROOT"
  fi
fi

# Make paths absolute for later use.
if ! $CLEAN; then
  SORTIX_MIRROR_DIR=$(make_dir_path_absolute "$SORTIX_MIRROR_DIR")
fi
SORTIX_PORTS_DIR=$(make_dir_path_absolute "$SORTIX_PORTS_DIR")
if [ "$OPERATION" = build ]; then
  SYSROOT=$(make_dir_path_absolute "$SYSROOT")
  SORTIX_REPOSITORY_DIR=$(make_dir_path_absolute "$SORTIX_REPOSITORY_DIR")
fi

# Decide the optimization options with which the ports will be built.
if [ -z "${OPTLEVEL+x}" ]; then OPTLEVEL="-Os -s"; fi
if [ -z "${PORTS_OPTLEVEL+x}" ]; then PORTS_OPTLEVEL="$OPTLEVEL"; fi
if [ -z "${PORTS_CFLAGS+x}" ]; then PORTS_CFLAGS="$PORTS_OPTLEVEL"; fi
if [ -z "${PORTS_CXXFLAGS+x}" ]; then PORTS_CXXFLAGS="$PORTS_OPTLEVEL"; fi
if [ -z "${CFLAGS+x}" ]; then CFLAGS="$PORTS_CFLAGS"; fi
if [ -z "${CXXFLAGS+x}" ]; then CXXFLAGS="$PORTS_CXXFLAGS"; fi
WERRORFORMAT="-Werror=format -Wno-error=format-contains-nul"
# TODO: After releasing Sortix 1.1, use these new options conditionally.
if [ "$OPERATION" = build ] && \
   ! "$HOST-gcc" --version | grep -Eq ' \(GCC\) 5\.2\.0$'; then
  WERRORFORMAT="$WERRORFORMAT -Wno-error=format-overflow -Wno-error=format-truncation"
fi
CFLAGS="$CFLAGS $WERRORFORMAT -Werror=implicit-function-declaration"
CXXFLAGS="$CXXFLAGS $WERRORFORMAT"
export CFLAGS
export CXXFLAGS

# Build and install all the packages.
tix-metabuild \
  ${BUILD:+--build="$BUILD"} \
  $CACHE_PACKAGE \
  ${SYSROOT:+--collection="$SYSROOT"} \
  ${SORTIX_REPOSITORY_DIR:+--destination="$SORTIX_REPOSITORY_DIR"} \
  $DISTCLEAN \
  ${END:+--end="$END"} \
  --generation=3 \
  ${HOST:+--host="$HOST"} \
  ${SORTIX_PORTS_MIRROR:+--mirror="$SORTIX_PORTS_MIRROR"} \
  ${SORTIX_MIRROR_DIR:+--mirror-directory="$SORTIX_MIRROR_DIR"} \
  --packages="${PACKAGES-all!!}" \
  --prefix= \
  $RANDOMIZE \
  ${START:+--start="$START"} \
  ${SYSROOT:+--sysroot="$SYSROOT"} \
  "$SORTIX_PORTS_DIR"
