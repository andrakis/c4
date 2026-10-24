#!/bin/sh
# games/fetch.sh FILE SHA256 VAR [URL...] -- a game's data, the file its
# c4.mk names, checked by its SHA-256: already there, or the copy $VAR
# names, or the first URL that gives exactly that file. A game's data is
# never committed (games/README.md).
set -e
f=$1; sum=$2; var=$3; shift 3
ok() { [ -f "$1" ] && [ "$(sha256sum "$1" | cut -d' ' -f1)" = "$sum" ]; }
ok "$f" && exit 0
eval "from=\${$var:-}"
mkdir -p "$(dirname "$f")"
if [ -n "$from" ]; then
    ok "$from" || { echo "$var=$from: not the file $(basename "$f") (its SHA-256 differs)" >&2; exit 1; }
    cp "$from" "$f"
    exit 0
fi
for u in "$@"; do
    if curl -sSfL -o "$f.part" "$u" && ok "$f.part"; then mv "$f.part" "$f"; echo "$(basename "$f"): from $u"; exit 0; fi
    rm -f "$f.part"
done
echo "$(basename "$f") is not here: set $var to your copy, or see $(dirname "$f")/README.md" >&2
exit 1
