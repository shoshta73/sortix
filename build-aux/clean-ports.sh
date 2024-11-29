set -e

make_dir_path_absolute() {
  (cd "$1" && pwd)
}

has_command() {
  which "$1" > /dev/null
}

# Detect if the environment isn't set up properly.
if [ -z "$SORTIX_PORTS_DIR" ]; then
  echo "$0: error: You need to set \$SORTIX_PORTS_DIR" >&2
  exit 1
elif ! [ -d "$SORTIX_PORTS_DIR" ] ||
     [ "$(ls "$SORTIX_PORTS_DIR") | wc -l" = 0 ]; then
  exit 0
elif ! has_command tix-vars; then
  echo "$0: warning: Can't clean ports directory without Tix locally installed." >&2
  exit 0
fi

# Make paths absolute for later use.
SORTIX_PORTS_DIR=$(make_dir_path_absolute "$SORTIX_PORTS_DIR")

# Clean all the packages.
for PACKAGE in $(tix-list-packages --ports="$SORTIX_PORTS_DIR" 'all!!'); do
  if [ "$1" = distclean ]; then
    DEVELOPMENT=$(tix-vars -d false $SORTIX_PORTS_DIR/$PACKAGE/$PACKAGE.port \
                           DEVELOPMENT)
    if [ "$DEVELOPMENT" = true ]; then
      case "$(cat "$SORTIX_PORTS_DIR/$PACKAGE/$PACKAGE.version")" in
      *.development)
        echo "Port is in development: '$SORTIX_PORTS_DIR/$PACKAGE/$PACKAGE'"
        continue
        ;;
      esac
    fi
    if [ -e "$SORTIX_PORTS_DIR/$PACKAGE/$PACKAGE" ]; then
      echo "Removing '$SORTIX_PORTS_DIR/$PACKAGE/$PACKAGE'"
    fi
    rm -rf "$SORTIX_PORTS_DIR/$PACKAGE/$PACKAGE"
    rm -rf "$SORTIX_PORTS_DIR/$PACKAGE/$PACKAGE.upstream"
    rm -f "$SORTIX_PORTS_DIR/$PACKAGE/$PACKAGE.version"
    rm -f "$SORTIX_PORTS_DIR/$PACKAGE/$PACKAGE.version.new"
  fi
  if [ -e "$SORTIX_PORTS_DIR/$PACKAGE/$PACKAGE.version" -o \
       -e "$SORTIX_PORTS_DIR/$PACKAGE/$PACKAGE.version.new" ]; then
    SOURCE_PORT=$(tix-vars -d '' $SORTIX_PORTS_DIR/$PACKAGE/$PACKAGE.port \
                           SOURCE_PORT)
    if [ -z "$SOURCE_PORT" ] ||
       [ -e "$SORTIX_PORTS_DIR/$SOURCE_PORT/$SOURCE_PORT" ]; then
      tix-build \
        --sysroot="/" \
        --prefix= \
        --destination="/" \
        --start=clean \
        --end=clean \
        ${SOURCE_PORT:+--source-port "$SORTIX_PORTS_DIR/$SOURCE_PORT/$SOURCE_PORT"} \
        "$SORTIX_PORTS_DIR/$PACKAGE/$PACKAGE"
    fi
  fi
done
