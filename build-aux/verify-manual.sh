#!/bin/sh
# Checks if mandoc -Tlint warns about any manual pages.
RESULT=true
for MANUAL in $(git ls-files | grep -E '\.[0-9]$' | grep -Ev '^libm/man/'); do
  # TODO: mandoc on Sortix can't parse .Dd dates at the moment.
  # TODO: mandoc really needs a way to turn warnings on/off.
  # TODO: Ignore warnings with known false positives that we don't know how to
  #       work around just yet.
  if mandoc -Tlint $MANUAL 2>&1 | grep -Ev 'WARNING: cannot parse date|STYLE: referenced manual not found|STYLE: consider using OS macro|STYLE: verbatim "--", maybe consider using \\\(em|STYLE: no blank before trailing delimiter|outdated mandoc.db lacks'; then
    RESULT=false
  fi
done
$RESULT
