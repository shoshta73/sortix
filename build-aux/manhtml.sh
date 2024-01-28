#!/bin/sh
set -e

export LC_ALL=C
SECTIONS="1 2 3 4 5 6 7 8 9"
if [ -z ${SORTIX_SITE_OFFICIAL+x} ]; then
  SORTIX_SITE_OFFICIAL="https://sortix.org/"
fi
if [ -z ${SORTIX_RELEASE_SITE+x} ]; then
  SORTIX_RELEASE_SITE="https://pub.sortix.org/sortix/release"
fi
if [ -z ${MANHTML_TO_SITE_ROOT+x} ]; then
  MANHTML_TO_SITE_ROOT=.
fi
if [ -z ${MANHTML_TEMPLATE+x} ]; then
  MANHTML_TEMPLATE="$(dirname -- "$(realpath -- "$(which -- "$0")")")"
fi
if [ -z ${MANHTML_RELEASES+x} ]; then
  MANHTML_RELEASES="current nightly"
fi
if [ -z ${RELEASE+x} ]; then
  RELEASE="current"
fi
if [ -z ${RELEASE_STRING+x} ]; then
  if [ "$RELEASE" = current ]; then
    RELEASE_STRING="Sortix"
  else
    RELEASE_STRING="Sortix $RELEASE"
  fi
fi

if [ $# -lt 1 ]; then
  echo "Usage: $0 mandir $#" >&2
  exit 1
fi

if [ ! -e "$1/ports.list" ]; then
  echo "$0: No ports.list in $1" >&2
  exit 1
fi

cd "$1"

backwards() {
  echo "$1" | tr -cd / | sed 's,/,../,g'
}

tositeofficial() {
  BACKWARDS=$(backwards "$1")
  case "$SORTIX_SITE_OFFICIAL" in
  http*) echo "$SORTIX_SITE_OFFICIAL" ;;
  *) echo "$BACKWARDS$SORTIX_SITE_OFFICIAL" ;;
  esac
}

tositeroot() {
  echo "$(backwards "$1")$MANHTML_TO_SITE_ROOT"
}

frontpage() {
  tositeofficial "$1" | sed 's/^$/./g'
}

begin_html() {
  cat "$MANHTML_TEMPLATE/header.html" | sed "s|@title@|$1|g"
}

header() {
  cat << EOF
<h1>$1</h1>
EOF
}

section() {
  cat << EOF
<h2 id="$2"><a href="#$2">$1</a></h2>
EOF
}

bp() {
  echo "<p>"
}

ep() {
  echo "</p>"
}

link() {
  cat << EOF
<a href="$1">$2</a>$3<br />
EOF
}

end_html() {
  cat "$MANHTML_TEMPLATE/footer.html"
}

finalize_html_stdout() {
  sed -e "s,@root@,$(tositeroot "$1"),g" \
      -e "s,@official@,$(tositeofficial "$1"),g" \
      -e "s,@frontpage@,$(frontpage "$1"),g" \
      -e "s,@year@,$(date +%Y),g"
}

finalize_html() {
  if [ "$VERBOSE" = 1 ]; then
    echo "$1" >&2
  fi
  finalize_html_stdout "$1" > "$1"
}

section_name() {
  case "$1" in
  1) echo "General Commands Manual";;
  2) echo "System Calls Manual";;
  3) echo "Library Functions Manual";;
  4) echo "Device Drivers Manual";;
  5) echo "File Formats Manual";;
  6) echo "Games Manual";;
  7) echo "Miscellaneous Information Manual";;
  8) echo "System Manager's Manual";;
  9) echo "Kernel Developer's Manual";;
  esac
}

# TODO: This doesn't work for headers in manual pages like addr2line(1) that
#       doesn't use mdoc(7), so the sections don't have id attributes.
selflink() {
  python3 -c '
import re
import sys

id_map = {}
first_pass = []
for line in sys.stdin.readlines():
    result = ""
    while True:
        match = re.search("(.*?)<h(.)( id=\"([^\">]*)\")?>(.*?)</h.>(.*)", line, flags=re.DOTALL)
        if match is None:
            result += line
            break
        lead = match.group(1)
        hl = match.group(2)
        oldid = match.group(4)
        h = match.group(5)
        rest = match.group(6)
        id = h.lower().replace(" ", "-").replace("%", "%25");
        if oldid is not None:
            id_map[oldid] = id
        result += lead + "<h" + hl + " id=\"" + id + "\"><a href=\"#" + id + "\">" + h + "</a></h" + hl + ">"
        line = rest
    first_pass.append(result)
for line in first_pass:
    result = ""
    while True:
        match = re.search("(.*?)<a class=\"link-sec\" href=\"#([^\">]*)\">(.*?)</a>(.*)", line, flags=re.DOTALL)
        if match is None:
            result += line
            break
        lead = match.group(1)
        id = match.group(2)
        text = match.group(3)
        rest = match.group(4)
        if id in id_map:
            id = id_map[id]
        result += lead + "<a class=\"link-sec\" href=\"#" + id + "\">" + text + "</a>"
        line = rest
    print(result, end="")
'
}

see_other_releases() {
  if [ "$2" = true ]; then
    echo '<div style="text-align: right; float: right;">'
  else
    bp
    echo "View in release:"
  fi
  for OTHER_RELEASE in $MANHTML_RELEASES; do
    if [ "$RELEASE" = "$OTHER_RELEASE" ]; then
      OTHER_RELEASE_URL=$(backwards "$1")"$1"
    elif [ "$OTHER_RELEASE" = current ]; then
      OTHER_RELEASE_URL=$(tositeofficial "$1")"man/$1"
	elif echo "$OTHER_RELEASE" | grep -Eq '^[0-9]+.[0-9]+$'; then
      OTHER_RELEASE_URL=$(tositeofficial "$1")"release/$OTHER_RELEASE/man/$1"
    else
      OTHER_RELEASE_URL="$SORTIX_RELEASE_SITE/$OTHER_RELEASE/man/$1"
    fi
    if [ "$OTHER_RELEASE" = "$RELEASE" ]; then
      printf ' <b><a href="%s">%s</a></b>' "$OTHER_RELEASE_URL" "$OTHER_RELEASE"
    else
      printf ' <a href="%s">%s</a>' "$OTHER_RELEASE_URL" "$OTHER_RELEASE"
    fi
  done
  if [ "$2" = true ]; then
    echo '</div>'
  else
    ep
  fi
}

see_stable() {
  if [ "$2" = true ]; then
    see_other_releases "$@"
  fi
  case "$RELEASE" in
  current)
    if [ "$2" != true ]; then
      see_other_releases "$@"
    fi
    ;;
  *dev* | nightly | volatile)
    if [ "$2" = true ]; then
      header "$RELEASE_STRING manual"
    else
      see_other_releases "$@"
    fi
    bp
    cat << EOF
This manual documents $RELEASE_STRING, a development build that has not
been officially released. You can instead view
<a href="@official@man/$1">this document in the latest official manual</a>.
EOF
    ep
    ;;
  *)
    if [ "$2" = true ]; then
      header "$RELEASE_STRING manual"
    else
      see_other_releases "$@"
    fi
    bp
    cat << EOF
This manual documents $RELEASE_STRING. You can instead view
<a href="@official@man/$1">this document in the latest official manual</a>.
EOF
    ep
    ;;
  esac
}

undocumented_see_also() {
  (cat && echo "../man1/man.1.html") |
  sort |
  (while read FILE; do
    NAME=$(expr x"$FILE" : x'.*/\([^/]*\)\.[0-9]\.html$')
    SECTION=$(expr x"$FILE" : x'.*\.\([0-9]\)\.html$')
    printf ".Xr $NAME $SECTION /"
   done) | sed -e 's| /$|\\|' -e 's|/|,\\|g' | tr '/' ',' | tr '\\' '\n'
}

echo "Generating manhtml index"
(begin_html "$RELEASE_STRING manual page index"
 header "$RELEASE_STRING manual page index"
 see_stable ""
 section "Introductory manual pages" "introductory"
 bp
 link "man7/installation.7.html" "installation(7)"
 link "man7/upgrade.7.html" "upgrade(7)"
 link "man7/user-guide.7.html" "user-guide(7)"
 link "man7/development.7.html" "development(7)"
 link "man7/cross-development.7.html" "cross-development(7)"
 if [ -f "man7/following-development.7"  ]; then
   link "man7/following-development.7.html" "following-development(7)"
 fi
 if [ -f "man7/porting.7"  ]; then
   link "man7/porting.7.html" "porting(7)"
 fi
 ep
 section "Sortix manual pages" "system"
 bp
 link "system.html" "System manual pages"
 for section in $SECTIONS; do
   link "system.html#$section" "System Section $section - $(section_name $section)"
 done
 section "All manual pages" "all"
 link "all.html" "All manual pages"
 for section in $SECTIONS; do
   link "man$section/" "Section $section - $(section_name $section)"
 done
 ep
 section "Ports manual pages" "ports"
 bp
 grep -Ev '^system$' ports.list | sort |
 while read port; do
   if [ -s "$port.list" ]; then
     link "$port.html" "$port"
   else
     link "$port.html" "$port" " (no manual pages)"
   fi
 done
 ep
 end_html) | finalize_html index.html

(echo system && grep -Ev '^system$' ports.list) |
while read port; do
  echo "Generating manhtml index for $port"
  (if [ "$port" = system ]; then prettyport="System"; else prettyport=$port; fi
   begin_html "$prettyport manual pages" &&
   header "$prettyport manual pages" &&
   see_stable "$port.html"
   section_not_begun=true
   for section in $SECTIONS; do
     section_not_begun=true
     mkdir -p "man$section"
     grep -E "^/share/man/man$section/.*\.$section$" "$port.list" | sort |
     while read manpage; do
       if $section_not_begun; then
         section "Section $section - $(section_name $section)" "$section"
         bp
         section_not_begun=false
       fi
       manpage=$(basename -- "$manpage")
       name=$(echo "$manpage" | sed "s/\.$section$//")
       link "man$section/$manpage.html" "$name($section)"
       if [ "x$port" = xsystem ]; then
         mandoc -Tlint "man$section/$manpage" 1>&2 || true
       fi
     done
     if ! $section_not_begun; then
       ep
     fi
   done
   if [ ! -s "$port.list" ]; then
     echo "<p>$prettyport contains no manual pages.</p>"
   fi
   end_html) | finalize_html $port.html
done

for section in $SECTIONS; do
  echo "Generating manhtml index for section $section"
  mkdir -p "man$section"
  (begin_html "Section $section - $(section_name $section)" &&
   header "Section $section - $(section_name $section)" &&
   see_stable "man$section/"
   ls man$section | sort | grep -E "\.$section$" |
   while read manpage; do
     manpage=$(basename -- "$manpage")
     name=$(echo "$manpage" | sed "s/\.$section$//")
     link "$manpage.html" "$name($section)"
   done
   if ! ls man$section | grep -Eq "\.$section$"; then
     echo "<p>Section $section contains no manual pages.</p>"
   fi
   end_html) | finalize_html man$section/index.html
done

for section in $SECTIONS; do
  find man$section -type f | sort | grep -E "\.$section$" |
  while read manpage; do
    filename=$(basename -- "$manpage")
    name=$(expr "x$filename" : 'x\(.*\)\.[^.]*')
    echo Generating manhtml $manpage
    (begin_html "$name($section)"
     see_stable "$manpage.html" true
     mandoc -Thtml -Ofragment,man=../man%S/%N.%S.html "$manpage" | selflink
     end_html) | finalize_html "$manpage.html"
  done
done

(find . -name '*.html' | while read FILE; do
   grep -Eoh 'href="../man[[:digit:]]/[^/]+\.html"' "$FILE" || true
 done) | sort -u | grep -Eo 'man[[:digit:]]/[^/]+\.html' | sed 's/\.html$//' |
while read manpage; do
  if ! [ -e "$manpage" ]; then
    echo "Generating undocumented manhtml for $manpage"
    filename=$(basename -- "$manpage")
    name=$(expr "x$filename" : 'x\(.*\)\.[^.]*')
    NAME=$(echo "$filename" | tr '[:lower:]' '[:upper:]')
    section=$(expr "x$manpage" : 'x.*\.\([^.]*\)')
    (begin_html "$name($section)"
     see_stable "$manpage.html" true
     (cat man7/undocumented.7 |
      grep -Ev '^\.Xr man 1$' |
      sed -e "s/UNDOCUMENTED/$NAME/g" -e "s/undocumented/$name/g" &&
      grep -rl -F -- "../$manpage.html" . |
      grep -E '\.html$' |
      undocumented_see_also) |
     mandoc -Thtml -Oman=../man%S/%N.%S.html | selflink
     end_html) | finalize_html "$manpage.html"
  fi
done

echo "Generating manhtml index for all"
(begin_html "All manual pages for Sortix"
 header "All manual pages for Sortix"
 see_stable "all.html"
 for section in $SECTIONS; do
   section "Section $section" "$section"
   bp
   ls man$section | sort | grep -E "\.$section$" |
   while read manpage; do
     name=$(echo "$manpage" | sed "s/\.$section$//")
     link "man$section/$manpage.html" "$name($section)"
   done
   ep
 done
 end_html) | finalize_html all.html
