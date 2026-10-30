#!/bin/sh
# libjs/site.sh OUT -- the c4m.js page as a static site, in OUT: what
# GitHub Pages serves (libjs/pages.sh), from a tree where
# make c4bb-images has run.
#
# The site is the repository's own layout, because the page imports from
# src/ and the Code Explorer shows the sources: every tracked file, the
# checked-out games' with them, and the images the build made
# (src/c4bb/images, libjs/images). A static host cannot add the headers
# that isolate the page; libjs/coi.js, a service worker, adds them.
#
# The games are the ones checked out (games/README.md), their data as
# make NAME-data fetched it. Tyrian's data may not be passed on
# (games/tyrian/data/README.md), so a tree with games/tyrian makes no
# site, and neither does one where any image holds a Tyrian file.
set -e
out=${1:?usage: libjs/site.sh OUT}
cd "$(dirname "$0")/.."
if [ -e games/tyrian/c4.mk ] || [ -n "$(find src/c4bb/images libjs/images -ipath '*tyrian*' | head -1)" ]; then
    echo "libjs/site.sh: Tyrian is in this tree, and its data may not be published" >&2
    exit 1
fi
[ -f src/c4bb/images/c4ix32.c4r ] || { echo "libjs/site.sh: no images here (make c4bb-images)" >&2; exit 1; }
rm -rf "$out"
mkdir -p "$out"
list=$(mktemp)
git ls-files --recurse-submodules | while IFS= read -r f; do
    if [ -f "$f" ]; then printf '%s\n' "$f"; fi
done > "$list"
tar -cf - --no-recursion -T "$list" | tar -xf - -C "$out"
rm -f "$list"
mkdir -p "$out/src/c4bb" "$out/libjs"
cp -r src/c4bb/images "$out/src/c4bb/"
cp -r libjs/images "$out/libjs/"
# the site's defaults are the tree's (libjs/web/site.js): the MMU off,
# as the page has it everywhere now (?mmu=1 fits one)
# the site's front door is the page
cat > "$out/index.html" <<'EOF'
<!DOCTYPE html>
<meta charset="utf-8">
<title>c4m.js</title>
<meta http-equiv="refresh" content="0; url=libjs/web/">
<a href="libjs/web/">c4m.js</a>
EOF
big=$(find "$out" -type f -size +100M)
if [ -n "$big" ]; then
    echo "libjs/site.sh: GitHub Pages takes no file over 100 MB: $big" >&2
    exit 1
fi
echo "libjs/site.sh: $out, $(du -sh "$out" | cut -f1), $(find "$out" -type f | wc -l) files"
