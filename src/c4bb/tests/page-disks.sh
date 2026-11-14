# page-disks.sh - the page disks' directories, sourced by build-images.sh
# and climb-disks.sh from the repo root. They use $GAMEFILES (a game's
# files, one list each, $OUT/.gamefiles) and $GAMESDISK.
#
# Both page disks are in directories (C4DOS's API v7, docs/c4dos-design.md
# "Directories"): every directory has its own c4dos.dir, a subdirectory a
# "NAME/" line in its parent's, made here by `listings`. lj_put puts a
# file onto one of them at a path, linked where the filesystem allows;
# games_put puts each game's files into GAMES/NAME.
listings () {           # listings DIR: a c4dos.dir in every directory under DIR
    (cd $1 && find . -type d | while read -r d; do
        (cd "$d" && { ls -p | grep '/$' || true; ls -p | grep -v '/$' | grep -v '^manifest.json$\|^index.json$' || true; } > c4dos.dir)
    done)
}
#
# These run once per file, thousands of times, so they start no process
# they can do without: no $(...), no grep per game. A fork is cheap on
# Linux, but under MSYS2 on Windows (docs/windows.md) it costs tens of
# milliseconds, and twenty a file made the ladder disk half an hour.
lj_put () {             # lj_put SRC DISK PATH
    case $3 in */*) set -- "$1" "$2" "$3" "$2/${3%/*}" ;; *) set -- "$1" "$2" "$3" "$2" ;; esac
    [ -d "$4" ] || mkdir -p "$4"
    rm -f "$2/$3"
    ln "$1" "$2/$3" 2> /dev/null || cp "$1" "$2/$3"
}
games_map () {          # games_map: GAME_OF[PATH], the game that put PATH on the games disk, for game_of
    declare -gA GAME_OF=()
    local gf f
    for gf in $GAMEFILES/*; do
        [ -f "$gf" ] || continue
        while IFS= read -r f || [ -n "$f" ]; do
            [ -n "$f" ] && [ -z "${GAME_OF[$f]+x}" ] && GAME_OF[$f]=${gf##*/}
        done < "$gf"
    done
    return 0
}
game_of () {            # game_of PATH: game= the game that put PATH on the games disk, or nothing (games_map first)
    game=${GAME_OF[$1]-}
}
games_put () {          # games_put DISK: each game's files into DISK/GAMES/NAME
    for gf in $GAMEFILES/*; do
        [ -f "$gf" ] || continue
        while read -r f; do
            if [ -f "$GAMESDISK/$f" ]; then lj_put "$GAMESDISK/$f" $1 "GAMES/${gf##*/}/$f"; fi
        done < $gf
    done
    return 0
}
# the games' directories, for APPEND: "\GAMES\DOOM;\GAMES\ROTT"
games_dirs () {
    for gf in $GAMEFILES/*; do if [ -s "$gf" ]; then printf '\\GAMES\\%s;' "$(basename $gf)"; fi; done | sed 's/;$//'
}
disk_indexes () {       # disk_indexes DISK: manifest.json and index.json, every file in every directory
    (cd $1 && find . -type f -not -name manifest.json | sed 's|^\./||') | \
        awk 'BEGIN{printf "["} NR>1{printf ","} {printf "\"%s\"", $0} END{print "]"}' > $1/manifest.json
    (cd $1 && find . -type f -not -name manifest.json -not -name index.json -printf '%s %P\n' | sort -k2) | \
        awk 'BEGIN{printf "["} NR>1{printf ","} {n=$0; sub(/^[0-9]+ /,"",n); printf "[\"%s\",%s]", n, $1} END{print "]"}' \
        > $1/index.json
}
