#!/bin/sh
set -e

umask 0022

# Avoid already-exported environment variables leaking into the build. All
# variables used by this script must be unset here before use.
unset build
unset build_id
unset cache_package
unset clean
unset distclean
unset download_packages
unset end
unset host
unset lean
unset mirror_dir
unset operation
unset packages
unset ports_dir
unset ports_mirror
unset randomize
unset release_url
unset repository_dir
unset signing_public_key
unset start
unset sysroot
unset werrorformat

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
operation="$1"

clean=false
case "$operation" in
distclean) clean=true; distclean=--distclean ;;
clean) clean=true; start=clean; end=clean ;;
download) end=download ;;
extract) end=extract ;;
build) cache_package=--cache-package; randomize=--randomize ;;
*) echo "$0: error: Invalid operation: $operation" >&2
   exit 1
esac

if [ "$DOWNLOAD_PACKAGES" = "yes" -a "$operation" = build ]; then
  download_packages=--download-package
fi

if [ "$LEAN" = "yes" ]; then
  lean=--lean
fi

# TODO: After releasing Sortix 1.1, remove support for building with Sortix 1.0
#       that doesn't have sort -R.
if [ -n "$randomize" ]; then
  if ! true | sort -R > /dev/null 2>&1; then
    randomize=
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
    echo "$0: warning: You need to have Tix installed to clean ports." >&2
    exit 0
  else
    echo "$0: error: You need to have Tix installed to compile ports." >&2
    exit 1
  fi
fi

# Import the parameter environment variables (uppercase) as internal
# non-exported variables (lowercase) that we can use to invoke tix-metabuild
# without it receiving these variables.
ports_dir=$SORTIX_PORTS_DIR
if [ -n "${BUILD+x}" ]; then build=$BUILD; fi
if [ -n "${BUILD_ID+x}" ]; then build_id=$BUILD_ID; fi
if [ -n "${HOST+x}" ]; then host=$HOST; fi
if [ -n "${PACKAGES+x}" ]; then packages=$PACKAGES; fi
if [ -n "${RELEASE_URL+x}" ]; then release_url=$RELEASE_URL; fi
if [ -n "${SIGNING_PUBLIC_KEY+x}" ]; then signing_public_key=$SIGNING_PUBLIC_KEY; fi
if [ -n "${SORTIX_MIRROR_DIR+x}" ]; then mirror_dir=$SORTIX_MIRROR_DIR; fi
if [ -n "${SORTIX_PORTS_MIRROR+x}" ]; then ports_mirror=$SORTIX_PORTS_MIRROR; fi
if [ -n "${SORTIX_REPOSITORY_DIR+x}" ]; then repository_dir=$SORTIX_REPOSITORY_DIR; fi
if [ -n "${SYSROOT+x}" ]; then sysroot=$SYSROOT; fi

if ! $clean; then
  # Create the mirror directory for downloaded archives.
  mkdir -p "$mirror_dir"

  # Add the platform triplet to the binary repository path.
  if [ "$operation" = build ]; then
    repository_dir="$repository_dir/$host"
    mkdir -p "$repository_dir"
  fi

  # Create the system root if absent.
  if [ "$operation" = build ]; then
    mkdir -p "$sysroot"
  fi
fi

# Make paths absolute for later use.
if ! $clean; then
  mirror_dir=$(make_dir_path_absolute "$mirror_dir")
fi
ports_dir=$(make_dir_path_absolute "$ports_dir")
if [ "$operation" = build ]; then
  sysroot=$(make_dir_path_absolute "$sysroot")
  repository_dir=$(make_dir_path_absolute "$repository_dir")
fi

# Decide the optimization options with which the ports will be built.
if [ -z "${OPTLEVEL+x}" ]; then OPTLEVEL="-Os -s"; fi
if [ -z "${PORTS_OPTLEVEL+x}" ]; then PORTS_OPTLEVEL="$OPTLEVEL"; fi
if [ -z "${PORTS_CFLAGS+x}" ]; then PORTS_CFLAGS="$PORTS_OPTLEVEL"; fi
if [ -z "${PORTS_CXXFLAGS+x}" ]; then PORTS_CXXFLAGS="$PORTS_OPTLEVEL"; fi
if [ -z "${CFLAGS+x}" ]; then CFLAGS="$PORTS_CFLAGS"; fi
if [ -z "${CXXFLAGS+x}" ]; then CXXFLAGS="$PORTS_CXXFLAGS"; fi
werrorformat="-Werror=format -Wno-error=format-contains-nul"
# TODO: After releasing Sortix 1.1, use these new options conditionally.
if [ "$operation" = build ] && \
   ! "$HOST-gcc" --version | grep -Eq ' \(GCC\) 5\.2\.0$'; then
  werrorformat="$werrorformat -Wno-error=format-overflow -Wno-error=format-truncation"
fi
CFLAGS="$CFLAGS $werrorformat -Werror=implicit-function-declaration"
CXXFLAGS="$CXXFLAGS $werrorformat"
export CFLAGS
export CXXFLAGS

# Avoid unintended environment variables leaking into the build. All variables
# used as parameters to this script must be unset here, as well as user
# parameters to the top-level makefile, unless they are explicitly intended for
# tix-metabuild.
unset BUILD
unset BUILD_ID
unset DOWNLOAD_PACKAGES
unset HOST
unset LEAN
unset PACKAGES
unset PORTS_CFLAGS
unset PORTS_CXXFLAGS
unset PORTS_OPTLEVEL
unset RELEASE_URL
unset SIGNING_KEY
unset SIGNING_KEY_SEARCH
unset SIGNING_PUBLIC_KEY
unset SIGNING_SECRET_KEY
unset SORTIX_MIRROR_DIR
unset SORTIX_PORTS_DIR
unset SORTIX_PORTS_MIRROR
unset SORTIX_REPOSITORY_DIR
unset SYSROOT
unset TARGET
unset CHANNEL
unset RELEASE
unset RELEASE_AUTHORITATIVE

# Build and install all the packages.
tix-metabuild \
  ${build:+--build="$build"} \
  ${build_id+--build-id="$build_id"} \
  $cache_package \
  ${sysroot:+--collection="$sysroot"} \
  ${repository_dir:+--destination="$repository_dir"} \
  $distclean \
  $download_packages \
  ${end:+--end="$end"} \
  --generation=3 \
  ${host:+--host="$host"} \
  ${lean} \
  ${ports_mirror:+--mirror="$ports_mirror"} \
  ${mirror_dir:+--mirror-directory="$mirror_dir"} \
  --packages="${packages-all!!}" \
  --prefix= \
  $randomize \
  ${signing_public_key+--release-key="$signing_public_key"} \
  ${release_url+--release-url="$release_url"} \
  ${start:+--start="$start"} \
  ${sysroot:+--sysroot="$sysroot"} \
  "$ports_dir"
