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
# make NAME-data fetched it. OpenTyrian, the engine, is GPL and may be
# published, but Tyrian 2.1's data may not (games/tyrian/data/README.md):
# the person brings it, through the page's Uploads (libjs/uploads.js).
# So no site is made where any file it would publish is one of Tyrian
# 2.1's, or a zip holds one: libjs/tyrian21.sha256 names them by SHA-256
# (a tree whose images were built with TYRIAN_ZIP has them). This is
# checked before OUT is touched.
set -e
out=${1:?usage: libjs/site.sh OUT}
cd "$(dirname "$0")/.."
[ -f src/c4bb/images/c4ix32.c4r ] || { echo "libjs/site.sh: no images here (make c4bb-images)" >&2; exit 1; }
list=$(mktemp)
git ls-files --recurse-submodules | while IFS= read -r f; do
    if [ -f "$f" ]; then printf '%s\n' "$f"; fi
done > "$list"
if ! python3 -I - libjs/tyrian21.sha256 "$list" src/c4bb/images libjs/images <<'EOF'
# every file the site would have (the list's, and those under the
# directories), and every file in each zip among them, against the sums
import hashlib, os, sys, zipfile
sums, listing, dirs = sys.argv[1], sys.argv[2], sys.argv[3:]
bad = {}
for l in open(sums):
    if l.strip() and not l.startswith('#'):
        h, n = l.split(None, 1)
        bad.setdefault(h, n.strip())
files = [l.rstrip('\n') for l in open(listing) if l.strip()]
for d in dirs:
    for root, _, names in os.walk(d):
        files += [os.path.join(root, n) for n in names]
found = []
for f in sorted(set(files)):
    if not os.path.isfile(f):
        continue
    h = hashlib.sha256()
    with open(f, 'rb') as fh:
        for b in iter(lambda: fh.read(1 << 20), b''):
            h.update(b)
    if h.hexdigest() in bad:
        found.append('%s (%s)' % (f, bad[h.hexdigest()]))
    elif zipfile.is_zipfile(f):
        try:
            z = zipfile.ZipFile(f)
            for i in z.infolist():
                if not i.is_dir() and hashlib.sha256(z.read(i)).hexdigest() in bad:
                    found.append('%s, which holds %s' % (f, i.filename))
                    break
        except Exception as e:
            if f.lower().endswith('.zip'):
                found.append('%s, a zip that could not be read to look (%s)' % (f, e))
if found:
    sys.stderr.write('libjs/site.sh: Tyrian 2.1\'s data may not be published, and the site would have it '
                     '(were the images built with TYRIAN_ZIP? make c4bb-images again without it):\n')
    for f in found[:10]:
        sys.stderr.write('  %s\n' % f)
    if len(found) > 10:
        sys.stderr.write('  ... and %d more\n' % (len(found) - 10))
    sys.exit(1)
EOF
then
    rm -f "$list"
    exit 1
fi
rm -rf "$out"
mkdir -p "$out"
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
