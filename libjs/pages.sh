#!/bin/sh
# libjs/pages.sh [BRANCH] -- the c4m.js page for GitHub Pages, built here
# and committed to the site's repository; pushing it is yours (make pages).
#
#   PAGES_TREE  the tree it builds in (default ~/git/c4-pages): a clone of
#               this repository with every game checked out that may be
#               published (GAMES, below): all but Super Blit Kart, the
#               licence on whose art is not settled. Tyrian is there as
#               OpenTyrian, the engine, without Tyrian 2.1's data, which
#               may not be passed on: the site's visitors bring their own
#               through the page's Uploads (libjs/uploads.js), and
#               libjs/site.sh refuses a site with any of it
#   PAGES_REPO  the site's repository (default ~/git/andrakis.github.io);
#               the site is its c4/, https://andrakis.github.io/c4/libjs/web/
#   PAGES_JOBS  make's -j (default 2)
#
# BRANCH (default: the one checked out here) is fetched from this
# repository, committed work only. The build tree keeps its objects
# between runs, so a run after the first remakes only what changed. The
# games' data comes from this tree's games/NAME/data where it is there,
# else from the games' MIRRORS (games/README.md), checked by SHA-256 --
# never Tyrian's (there is no tyrian-data; TYRIAN_ZIP set stops this
# here). Quake's is its whole shareware zip (QUAKE_AS_ZIP=1), which the
# page unpacks.
#
# The site repository is built by Jekyll (its _config.yml), which keeps
# a file without front matter as it is, but:
# - leaves out any name starting with . (the site has none: .gitignore
#   and the like are left out here) or _, unless _config.yml's include
#   names it (it has _*.h, for Rise of the Triad's headers): a name of
#   the site's it would leave out stops this here;
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
[ -z "${TYRIAN_ZIP+x}" ] || die "TYRIAN_ZIP is set: Tyrian 2.1's data may not be published (unset it)"
# the games: every one but sbk
GAMES="wolf3d doom rott quake duke3d frotz advent puzzles ace bsdgames pdcurses tyrian"

# the build tree, at BRANCH
[ -d "$tree/.git" ] || git clone -q --no-checkout "$here" "$tree"
git -C "$tree" fetch -q "$here" "$branch"
git -C "$tree" checkout -q --detach FETCH_HEAD
for g in $GAMES; do
    u=$(git -C "$here" config "submodule.$g.url" || true)
    if [ -n "$u" ]; then git -C "$tree" config "submodule.$g.url" "$u"; fi
done
# (.gitmodules says update = none for every game: checkout, here)
git -C "$tree" -c protocol.file.allow=always $(for g in $GAMES; do printf ' -c submodule.%s.update=checkout' $g; done) \
    submodule update -q --init --checkout $(for g in $GAMES; do printf ' games/%s' $g; done)
# a tree that had Super Blit Kart checked out by an older run
if [ -e "$tree/games/sbk/c4.mk" ]; then git -C "$tree" submodule deinit -q -f games/sbk; fi
# Tyrian's data directory as committed (its README), nothing more
extra=$(git -C "$tree/games/tyrian" status --porcelain --ignored -- data)
[ -z "$extra" ] || die "$tree/games/tyrian/data has more than its README: Tyrian 2.1's data may not be published: $extra"
rev=$(git -C "$tree" rev-parse --short HEAD)

# the games' data: this tree's copies, else their MIRRORS
cd "$tree"
have() { if [ -f "$here/games/$1" ]; then echo "$here/games/$1"; fi; }
WOLF3D_ZIP=${WOLF3D_ZIP:-$(have wolf3d/data/1wolf14.zip)} \
DOOM_WAD=${DOOM_WAD:-$(have doom/data/doom1.wad)} \
ROTT_ZIP=${ROTT_ZIP:-$(have rott/data/1rott13.zip)} \
QUAKE_ZIP=${QUAKE_ZIP:-$(have quake/data/quake106.zip)} \
DUKE3D_ZIP=${DUKE3D_ZIP:-$(have duke3d/data/3dduke13.zip)} \
ZORK1_Z3=${ZORK1_Z3:-$(have frotz/data/zork1.z3)} \
ZORK2_Z3=${ZORK2_Z3:-$(have frotz/data/zork2.z3)} \
ZORK3_Z3=${ZORK3_Z3:-$(have frotz/data/zork3.z3)} \
    make -s wolf3d-data doom-data rott-data quake-data duke3d-data frotz-data
echo "libjs/pages.sh: building the images in $tree at $branch ($rev)"
QUAKE_AS_ZIP=1 make -s -j"${PAGES_JOBS:-2}" c4bb-images

# the site, in the site's repository
sh libjs/site.sh "$repo/c4"
find "$repo/c4" -name '*.md' -delete
find "$repo/c4" -mindepth 1 -name '.*' -prune -exec rm -rf {} +
hidden=$(cd "$repo/c4" && find . -name '_*' ! -name '_*.h' | head -5 | tr '\n' ' ')
[ -z "$hidden" ] || die "Jekyll would leave these out of the site (_config.yml's include): $hidden"
grep -q '_\*\.h' "$repo/_config.yml" || die "$repo/_config.yml does not include _*.h (Rise of the Triad's headers)"
cd "$repo"
git add -A c4
if git diff --cached --quiet -- c4; then
    echo "libjs/pages.sh: the site is as it was; nothing to commit"
    exit 0
fi
printf 'c4: the c4m.js page at c4 %s (%s)\n\nBuilt by c4'"'"'s libjs/pages.sh, with the games that may be published:\n%s; Tyrian\nwith the visitor'"'"'s own data (Uploads).\n%s' \
    "$rev" "$branch" "$GAMES" "${PAGES_TRAILER:+
$PAGES_TRAILER
}" | git commit -q -F - -- c4
echo "libjs/pages.sh: committed $(git rev-parse --short HEAD) in $repo, $(du -sh c4 | cut -f1); git -C $repo push to publish it"
