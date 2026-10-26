#!/bin/sh
# libjs/pages.sh [BRANCH] -- the c4m.js page for GitHub Pages, built here
# and committed to the site's repository; pushing it is yours (make pages).
#
#   PAGES_TREE  the tree it builds in (default ~/git/c4-pages): a clone of
#               this repository with only Wolfenstein 3D, Doom and Rise of
#               the Triad checked out, never Tyrian, whose data may not be
#               passed on (libjs/site.sh refuses a tree with it)
#   PAGES_REPO  the site's repository (default ~/git/andrakis.github.io);
#               the site is its c4/, https://andrakis.github.io/c4/libjs/web/
#   PAGES_JOBS  make's -j (default 2)
#
# BRANCH (default: the one checked out here) is fetched from this
# repository, committed work only. The build tree keeps its objects
# between runs, so a run after the first remakes only what changed. The
# games' data comes from this tree's games/NAME/data where it is there,
# else from the games' MIRRORS (games/README.md), checked by SHA-256.
#
# The site repository is built by Jekyll (its _config.yml), which keeps
# a file without front matter as it is, but:
# - leaves out any name starting with _ or ., unless _config.yml's
#   include names it (it has _*.h, for Rise of the Triad's headers): a
#   name of the site's it would leave out stops this here;
# - makes every .md a page (jekyll-optional-front-matter), Liquid and
#   all, so the site has none: the page reads none of them.
set -e
die() { echo "libjs/pages.sh: $*" >&2; exit 1; }
here=$(cd "$(dirname "$0")/.." && pwd)
branch=${1:-$(git -C "$here" rev-parse --abbrev-ref HEAD)}
tree=${PAGES_TREE:-$HOME/git/c4-pages}
repo=${PAGES_REPO:-$HOME/git/andrakis.github.io}
[ -d "$repo/.git" ] || die "$repo is not the site's repository (PAGES_REPO)"
[ -z "$(git -C "$repo" status --porcelain -- c4)" ] || die "$repo/c4 has changes of its own: commit or drop them first"

# the build tree, at BRANCH
[ -d "$tree/.git" ] || git clone -q --no-checkout "$here" "$tree"
git -C "$tree" fetch -q "$here" "$branch"
git -C "$tree" checkout -q --detach FETCH_HEAD
for g in wolf3d doom rott; do
    u=$(git -C "$here" config "submodule.$g.url" || true)
    if [ -n "$u" ]; then git -C "$tree" config "submodule.$g.url" "$u"; fi
done
git -C "$tree" -c protocol.file.allow=always submodule update -q --init --checkout games/wolf3d games/doom games/rott
rev=$(git -C "$tree" rev-parse --short HEAD)

# the games' data: this tree's copies, else their MIRRORS
cd "$tree"
have() { if [ -f "$here/games/$1" ]; then echo "$here/games/$1"; fi; }
WOLF3D_ZIP=${WOLF3D_ZIP:-$(have wolf3d/data/1wolf14.zip)} \
DOOM_WAD=${DOOM_WAD:-$(have doom/data/doom1.wad)} \
ROTT_ZIP=${ROTT_ZIP:-$(have rott/data/1rott13.zip)} \
    make -s wolf3d-data doom-data rott-data
echo "libjs/pages.sh: building the images in $tree at $branch ($rev)"
make -s -j"${PAGES_JOBS:-2}" c4bb-images

# the site, in the site's repository
sh libjs/site.sh "$repo/c4"
find "$repo/c4" -name '*.md' -delete
hidden=$(cd "$repo/c4" && find . -name '[_.]*' ! -name '_*.h' ! -name . | head -5)
[ -z "$hidden" ] || die "Jekyll would leave these out of the site (_config.yml's include): $hidden"
grep -q '_\*\.h' "$repo/_config.yml" || die "$repo/_config.yml does not include _*.h (Rise of the Triad's headers)"
cd "$repo"
git add -A c4
if git diff --cached --quiet -- c4; then
    echo "libjs/pages.sh: the site is as it was; nothing to commit"
    exit 0
fi
printf 'c4: the c4m.js page at c4 %s (%s)\n\nBuilt by c4'"'"'s libjs/pages.sh, with Wolfenstein 3D, Doom and Rise of\nthe Triad.\n%s' \
    "$rev" "$branch" "${PAGES_TRAILER:+
$PAGES_TRAILER
}" | git commit -q -F - -- c4
echo "libjs/pages.sh: committed $(git rev-parse --short HEAD) in $repo, $(du -sh c4 | cut -f1); git -C $repo push to publish it"
