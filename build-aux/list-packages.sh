#!/bin/sh
set -e

if [ -z "$SORTIX_PORTS_DIR" ]; then
  SORTIX_PORTS_DIR="$(dirname -- "$0")/../ports"
fi

RUNTIME_DEPS=RUNTIME_DEPS

get_all_packages() {(
  for package in $(ls "$SORTIX_PORTS_DIR"); do
    if [ -f "$SORTIX_PORTS_DIR/$package/$package.port" ]; then
      echo $package
    fi
  done
)}

get_package_dependencies_raw() {(
  if [ -f "$SORTIX_PORTS_DIR/$1/$1.port" ]; then
    tix-vars -d ''  "$SORTIX_PORTS_DIR/$1/$1.port" BUILD_LIBRARIES $RUNTIME_DEPS
  fi
)}

# TODO: This algorithm scales extremely poorly.
get_package_dependencies_recursive() {(
  for dependency in $(get_package_dependencies_raw $1); do
    want=false
    if [ "$dependency" = '*' ]; then
       get_all_packages | grep -Ev '^all$'
       continue
    fi
    if [ "$2" = "!!" ]; then
      want=true
    else
      case "$dependency" in
      *"?") ;;
      *) want=true ;;
      esac
    fi
    if $want; then
      dependency=$(echo "$dependency" | tr -d '?')
      # Optional dependencies might not exist yet.
      if [ -f "$SORTIX_PORTS_DIR/$dependency/$dependency.port" ]; then
        echo "$dependency"
        get_package_dependencies_recursive "$dependency" "$2"
      fi
    fi
  done
)}

list_dependencies() {(
  package="$1"
  for dependency in $(get_package_dependencies_raw "$package"); do
    if [ "$dependency" != "${dependency%\?}" ]; then
      dependency="${dependency%\?}"
      for candidate in $PACKAGES; do
        if [ "$candidate" = "$dependency" ]; then
          echo "$dependency"
          break
        fi
      done
    else
      echo "$dependency"
    fi
  done
)}

list_package() {(
  package="$1"
  # Fast path for listing all packages.
  if [ "$package" = "all!" -o "$package" = "all!!" ]; then
    get_all_packages
    exit
  fi
  recursion=$(echo "$package" | grep -Eo '!*$')
  package=$(echo "$package" | grep -Eo '^[^!]*')
  echo "$package"
  if [ -n "$recursion" ]; then
    get_package_dependencies_recursive "$package" "$recursion"
  fi
)}

if [ "$1" = "--dependencies" ]; then
  shift
  RUNTIME_DEPS=
  PACKAGES=$("$0" PACKAGES)
  for package; do
    list_dependencies "$package"
  done | sort -u
  exit
fi

for package; do
  if [ "$package" = PACKAGES ]; then
    for package in ${PACKAGES-all!}; do
      list_package "$package"
    done
  else
    list_package "$1"
  fi
done | sort -u
