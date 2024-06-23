#!/bin/sh
# Verifies whether the system headers compiles on supported mixes of
# architectures, language standard versions, and feature macros.
set -e
if [ "$#" = 0 ]; then
  set i686-sortix x86_64-sortix
fi
for target; do
  for feature in "" "-D_POSIX_C_SOURCE=202405L" "-D_XOPEN_SOURCE=800" "-D_SORTIX_SOURCE"; do
    for std in \
      "-ansi -pedantic-errors" \
      "-std=c89 -pedantic-errors" \
      "-std=c90 -pedantic-errors" \
      "-std=c99 -pedantic-errors" \
      "-std=c11 -pedantic-errors" \
      "-std=c89" \
      "-std=gnu89" \
      "-std=c90" \
      "-std=gnu90" \
      "-std=c99" \
      "-std=gnu99" \
      "-std=c11" \
      "-std=gnu11" \
      "-std=c++98" \
      "-std=gnu++98" \
      "-std=c++11" \
      "-std=gnu++11" \
      "-std=c++14" \
      "-std=gnu++14" \
      ; do
      case "$std $feature" in
      *pedantic*-D_POSIX_SOURCE* | \
      *pedantic*-D_POSIX_C_SOURCE* | \
      *pedantic*-D_XOPEN_SOURCE* | \
      *pedantic*-D_SORTIX_SOURCE*)
        continue ;;
      esac
      echo "$(dirname -- "$0")/verify-headers-in-configuration.sh" "$target" "\"$std\"" "\"$feature\""
      "$(dirname -- "$0")/verify-headers-in-configuration.sh" "$target" "$std" "$feature"
    done
  done
  # TODO: Also verify kernel c++ headers.
done
