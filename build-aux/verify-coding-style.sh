#!/bin/sh
# Verifies the coding style conventions are adhered to.

RESULT=true

complain() {
  echo "$1 $2"
}

check_trailing_whitespace() {
  grep -q -E '(	| )+$' -- "$1"
}

check_tabs_following_spaces() {
  grep -q -E '^(	| )* +	' -- "$1"
}

check_trailing_blank_lines() {
  [ "$(cat -- "$1" | wc -l)" != "0" ] &&
  [ -z "$(tail -1 -- "$1")" ]
}

grep_copyright_header_full() {
  grep -m 1 -A 100 -E '^/\*+$' |
  grep -m 1 -B 100 -E '^\*+/$'
}

grep_copyright_header_contents() {
  grep -v -E '^/\*+$' |
  grep -v -E '^\*+/$'
}

has_leading_tabs() {
  grep -q -E '^	+'
}

has_bsd_tag() {
  grep -q -E '/\*[[:space:]]*\$.*\$[[:space:]]*\*/'
}

verify_source() {
  FILE="$1"
  if ! cat -- "$FILE" | has_bsd_tag &&
     ! grep -q -- "Permission to use, copy, modify, and distribute this software for any" "$FILE" &&
     ! grep -q -- "$FILE" "$FILE"; then
    complain "$PWD/$FILE" "doesn't contain its own file path"
    RESULT=false
  fi
  if ! grep -q -i -E 'copyright|public domain' -- "$FILE"; then
    complain "$PWD/$FILE" "doesn't have a copyright statement"
    RESULT=false
  fi
  if grep -q -E '^/\*{78}$' -- "$FILE" ||
     grep -q -E '^\*{78}/$' -- "$FILE"; then
    complain "$PWD/$FILE" "has an obsolete copyright statement"
    RESULT=false
  fi
  if grep -q -E '^/\*{80}$' -- "$FILE" ||
     grep -q -E '^/\*{80}$' -- "$FILE"; then
    complain "$PWD/$FILE" "has a spurious copyright statement"
    RESULT=false
  fi
  if cat -- "$FILE" |
     grep_copyright_header_full |
     grep_copyright_header_contents |
     has_leading_tabs; then
    complain "$PWD/$FILE" "has tabs in its copyright statement"
    RESULT=false
  fi
  if check_trailing_whitespace "$FILE"; then
    complain "$PWD/$FILE" "has trailing whitespace"
    RESULT=false
  fi
  if check_tabs_following_spaces "$FILE"; then
    complain "$PWD/$FILE" "has tabs following spaces"
    RESULT=false
  fi
  if check_trailing_blank_lines "$FILE"; then
    complain "$PWD/$FILE" "has trailing blank lines"
    RESULT=false
  fi
  if echo "$FILE" | grep -Eq 'include' &&
     grep -E '^#(define|ifndef)' -- "$FILE" | head -2 | grep -q '	'; then
    complain "$PWD/$FILE" "has tabs in include guards"
    RESULT=false
  fi
  # TODO: Wrong include guards.
  # TODO: Include guards being set to 1.
  $RESULT
}

# TODO: Some of these patterns should also apply to build-aux, and maybe also
#       partially to libm. Makefile and kblayout should also not have whitespace
#       problems.
for MODULE in $(git ls-files | grep / | sed 's,/.*,,' | sort -u | grep -Ev '^(build-aux|etc|libm|ports|share)$'); do
  cd "$MODULE"
  for FILE in $(git ls-files | grep -Ev '^include/' | grep -Ev '((^|/)(Makefile|\.gitignore|tixbuildinfo)|\.([0-9]|kblayout|f16|rgb))$'); do
    verify_source "$FILE"
  done
  if [ -e "$MODULE/include" ]; then
    cd include
    for FILE in $(cd $MODULE/include && git ls-files); do
      verify_source "$FILE"
    done
    cd ..
  fi
  cd ..
done

$RESULT
