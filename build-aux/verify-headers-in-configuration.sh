#!/bin/sh
# Verifies whether the system headers compiles with the mix of architecture,
# language standard version, and feature macros.
set -e
target="$1"
case $target in
i686-sortix) libm_machine=libm/arch/i387 ;;
x86_64-sortix) libm_machine=libm/arch/x86_64 ;;
esac
std="$2"
feature="$3"
(printf '.PHONY: all\n'
 printf 'all:\n'
 for header in \
   $(find libc/include -type f | sort) \
   $(find libm/include -type f | sort) \
   $(find kernel/include -type f | grep -Ev '^kernel/include/sortix/kernel/'| sort); do
   case $std in
   *-ansi*pedantic* | \
   *89*pedantic* | \
   *90*pedantic*)
     case $header in
     libc/include/assert.h | \
     libc/include/ctype.h | \
     libc/include/errno.h | \
     libc/include/limits.h | \
     libc/include/locale.h | \
     libc/include/setjmp.h | \
     libc/include/signal.h | \
     libc/include/stdarg.h | \
     libc/include/stddef.h | \
     libc/include/stdlib.h | \
     libc/include/string.h | \
     libm/include/float.h | \
     libm/include/math.h) ;;
     *) continue ;;
     esac ;;
     # TODO: Unsupported because fpos_t and time_t must be long long.
     #       These headers could use typedef __extension__ long long foo;
     #libc/include/stdio.h | \
     #libc/include/time.h | \
   *99*pedantic*)
     case $header in
     libc/include/assert.h | \
     libc/include/ctype.h | \
     libc/include/errno.h | \
     libc/include/inttypes.h | \
     libc/include/iso646.h | \
     libc/include/limits.h | \
     libc/include/locale.h | \
     libc/include/setjmp.h | \
     libc/include/signal.h | \
     libc/include/stdarg.h | \
     libc/include/stdbool.h | \
     libc/include/stddef.h | \
     libc/include/stdint.h | \
     libc/include/stdio.h | \
     libc/include/stdlib.h | \
     libc/include/string.h | \
     libc/include/time.h | \
     libc/include/wchar.h | \
     libc/include/wctype.h | \
     libm/include/complex.h | \
     libm/include/float.h | \
     libm/include/fenv.h | \
     libm/include/math.h | \
     libm/include/tgmath.h) ;;
     *) continue ;;
     esac ;;
   *11*pedantic*)
     case $header in
     libc/include/assert.h | \
     libc/include/ctype.h | \
     libc/include/errno.h | \
     libc/include/inttypes.h | \
     libc/include/iso646.h | \
     libc/include/limits.h | \
     libc/include/locale.h | \
     libc/include/setjmp.h | \
     libc/include/signal.h | \
     libc/include/stdalign.h | \
     libc/include/stdarg.h | \
     libc/include/stdatomic.h | \
     libc/include/stdbool.h | \
     libc/include/stddef.h | \
     libc/include/stdint.h | \
     libc/include/stdio.h | \
     libc/include/stdlib.h | \
     libc/include/stdnoreturn.h | \
     libc/include/string.h | \
     libc/include/threads.h | \
     libc/include/time.h | \
     libc/include/uchar.h | \
     libc/include/wchar.h | \
     libc/include/wctype.h | \
     libm/include/complex.h | \
     libm/include/float.h | \
     libm/include/fenv.h | \
     libm/include/math.h | \
     libm/include/tgmath.h) ;;
     *) continue ;;
     esac ;;
   esac
   printf 'all: %s\n' "$header"
   printf '.PHONY: %s\n' "$header"
   printf '%s:\n' "$header"
   case $std in
   *++*)
     printf '\t@%s\n' "$target-g++ $std $feature -c $header -o /dev/zero -O3 -Wall -Wextra -Wsystem-headers -Werror -I libc/include -I libm/include -I $libm_machine -I kernel/include"
     ;;
   *)
     printf '\t@%s\n' "$target-gcc $std $feature -c $header -o /dev/zero -O3 -Wall -Wextra -Wsystem-headers -Werror -I libc/include -I libm/include -I $libm_machine -I kernel/include"
     ;;
  esac
 done) | make -f - --no-print-directory
