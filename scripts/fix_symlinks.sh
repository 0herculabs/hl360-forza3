#!/bin/sh
# Replaces git symlinks (checked out as text files on Windows) with copies of their targets.
cd "$(dirname "$0")/../thirdparty/rexglue-sdk" || exit 1
git submodule foreach --quiet --recursive 'pwd' | while read -r repo; do
  ( cd "$repo" && git ls-files -s | awk '$1=="120000"{ $1=$2=$3=""; sub(/^ +/,""); print }' | while read -r link; do
      target=$(cat "$link")
      src="$(dirname "$link")/$target"
      if [ -e "$src" ]; then rm -f "$link"; cp -r "$src" "$link"; echo "fixed $repo/$link"; else echo "MISSING $repo/$link -> $target"; fi
    done )
done
