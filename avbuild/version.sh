#!/bin/sh

# check for git short hash
cnt=$(cd "$1" && git rev-list --count evolution)
hsh=$(cd "$1" && git rev-parse HEAD | cut -c1-7)

revision="$cnt-$hsh"

# releases extract the version number from the VERSION file
version=$(cd "$1" && cat VERSION 2> /dev/null)
test "$version" && version=$version-$revision || version=$revision

if [ -z "$2" ]; then
    echo "$version"
    exit
fi

NEW_REVISION="#define LIBAV_VERSION \"$version\""
OLD_REVISION=$(cat "$2" 2> /dev/null)

# Update version.h only on revision changes to avoid spurious rebuilds
if test "$NEW_REVISION" != "$OLD_REVISION"; then
    echo "$NEW_REVISION" > "$2"
fi
