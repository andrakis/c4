#!/bin/bash
# climb-disks.sh - the disks that are copies of the climb floppy
# (c4dos-c4ix32): the browser's climb disk, $OUT/climb, and the page's
# ladder disk, libjs/images/ladder, which lays the floppy over the shared
# disk and under the page's C4DOS disk.
#
# The floppy and build-images.sh feed each other: the floppy carries the
# C4IX userland and vfsload that script builds, and these disks copy the
# floppy. So build-images.sh sources this as its last copies, and the
# Makefile's c4bb-images runs it again on its own once it has remade the
# floppy from what build-images.sh built (docs/c4dos-ladder.md). Run from
# the repo root.
if [ -z "$OUT" ]; then          # on its own, not sourced
    set -e
    OUT=src/c4bb/images
    DISK=$OUT/disk
    LJDOS=libjs/images/dos
    GAMES_ON_DISK=$(cat $OUT/.games-on-disk 2> /dev/null || true)
    GAMEFILES=$OUT/.gamefiles
    GAMESDISK=$OUT/games
    say () { echo "c4bb: $*"; }
    . src/c4bb/tests/page-disks.sh
fi
# --- the climb disk, for the browser ---------------------------------
# The web front-end fetches media out of images/, and the whole ladder
# lives on c4dos-c4ix32 at the repo root, which it cannot reach. So a
# copy, with the manifest the browser reads. It is the SAME disk the
# CLI climb test boots, deliberately: two ladders that differ by which
# front-end you used would be one ladder and one demo.
CLIMBDISK=$OUT/climb
rm -rf $CLIMBDISK
CLIMBSRC=
for root in ../../../c4dos-c4ix32 c4dos-c4ix32; do
    if [ -d $root ]; then
        mkdir -p $CLIMBDISK
        cp -r $root/. $CLIMBDISK/
        CLIMBSRC=$root
        break
    fi
done
if [ -d $CLIMBDISK ]; then
    # SAY SO IF WHAT WE JUST COPIED IS OLDER THAN ITS SOURCE.
    #
    # This block COPIES; it does not build. c4dos-c4ix32 is made by its
    # own Makefile target, and nothing here ever checked whether that
    # target had been run since the sources changed. So a person who
    # rebuilt "the images" got a fresh corpus and a two-day-old climb
    # disk, booted the BIOS, and found a C4DOS that had never heard of
    # the drive letters that landed yesterday -- with nothing anywhere
    # saying which half was stale. That is a whole afternoon.
    #
    # A warning rather than an error: rebuilding the climb disk is
    # expensive (it builds c4sc), this script has other reasons to run,
    # and a person who knows the ladder is stale is allowed to carry on.
    # Against the SOURCE image, never against the copy: cp gives the
    # copy this moment's timestamp, so comparing with it says "fresh"
    # for a disk built any time at all. (Written that way first, which
    # is why the note is here.)
    # (Not when the Makefile's c4bb-images ran build-images.sh: it remakes
    # the floppy next and runs this again, C4BB_CLIMB_AGAIN says so.)
    for src in src/c4dos/c4dos.c src/c4ke/c4ke.c src/c4ix/sched.c; do
        [ -z "$C4BB_CLIMB_AGAIN" ] || break
        [ -f "$src" ] || continue
        [ -n "$CLIMBSRC" ] && [ -f "$CLIMBSRC/c4dos32.c4r" ] || continue
        if [ "$src" -nt "$CLIMBSRC/c4dos32.c4r" ]; then
            echo "c4bb: WARNING: the climb disk is older than $src." >&2
            echo "c4bb:          It is COPIED here, not built -- 'make c4bb-images' remakes" >&2
            echo "c4bb:          it and copies it again, or the browser's BIOS boot" >&2
            echo "c4bb:          will keep running the old ladder." >&2
            break
        fi
    done
    rm -f $CLIMBDISK/manifest.json
    (cd $CLIMBDISK && find . -type f -not -name manifest.json | sed 's|^\./||') | \
        awk 'BEGIN{printf "["} NR>1{printf ","} {printf "\"%s\"", $0} END{print "]"}' > $CLIMBDISK/manifest.json
    echo "c4bb: browser climb disk: $CLIMBDISK ($(ls $CLIMBDISK | wc -l) entries)"
fi

# ---- the page's ladder disk: libjs/images/ladder ----------------------------
# What ?system=c4dos-ladder boots: C4DOS with the games, and everything the
# climb needs, so one page session goes C4DOS -> C4KE -> C4IX with no
# reboot (docs/c4dos-ladder.md). Three layers, each over the one before:
#   - the shared disk's C4IX userland and its tree (c4ix.vfs.txt), so the
#     C4IX the machine builds boots into what ?system=c4ix has, desktop
#     and games included;
#   - the climb floppy (c4dos-c4ix32): the build kit, BUILD and LADDER's
#     tools, the sources as archives, b4ke, c4ix.b4k, kboot, and the C4KE
#     tree (c4ke.vfs.txt) that floppy is made for;
#   - the page's C4DOS disk: the games and their data, grin, and the
#     newest C4DOS.
# The prebuilt C4IX kernel is left out: the one that boots is the one b4ke
# links. Copied from the climb floppy, as $CLIMBDISK is, so it is skipped
# (and says so) when that floppy has not been made.
LJLAD=libjs/images/ladder
rm -rf $LJLAD
if [ -n "$CLIMBSRC" ] && [ -f $LJDOS/c4dos32.c4r ]; then
    say "the page's ladder disk, C4DOS to C4KE to C4IX: $LJLAD"
    mkdir -p $LJLAD
    # Its directories, a file's by its name: GAMES\NAME (a game's files,
    # as on the page's C4DOS disk), GRIN and BIN (the same), BUILD (the
    # build kit: the batch files, the archives, the compilers and the tools
    # BUILD, LADDER and IX run, c4lc's Lisp, b4ke and what it reads), C4IX
    # (C4IX's userland and its tree, c4ix.vfs.txt), and C4KE (C4KE's: the
    # rest of the shared disk and the climb floppy, its programs and their
    # sources). src/ stays where it was: the nested c4m innerbench starts
    # opens src/c4ke/c4ke.c by that path.
    lad_dir () {
        case "$1" in
            # (the shared disk's tyrian/ is C4IX's: its hostdir(tyrian/))
            tyrian/*) echo C4IX ;;
            */*) g=$(game_of "$1"); if [ -n "$g" ]; then echo "GAMES/$g"; fi ;;
            config.sys|autoexec.bat|c4dos32.c4r|ladder.txt) ;;
            grin.c4r|clock.c4r|calc.c4r|notepad.c4r|exitkey.c4r) echo GRIN ;;
            askline.c4r|dosfilt.c4r|hello.c4r) echo BIN ;;
            c4ix-src.tar|c4ix.b4k|c4ix.objs|c4ix.lst) echo BUILD ;;
            c4ix-*|c4ix.*|c4ix_user.h) echo C4IX ;;
            *.bat|*.tar|*.lisp|*.b4k|*.lst|u0.h) echo BUILD ;;
            cpp.c4r|c4cc.c4r|c4sp.c4r|c4sc.c4r|c4rlink.c4r|c4rdump.c4r|c4th.c4r) echo BUILD ;;
            dostar.c4r|dosload.c4r|b4ke.c4r|kboot.c4r|tar.c4r|install.c4r|kinstall.c4r) echo BUILD ;;
            bbsave.c4r|save.c4r|reboot.c4r) echo BUILD ;;
            *) g=$(game_of "$1"); if [ -n "$g" ]; then echo "GAMES/$g"; else echo C4KE; fi ;;
        esac
    }
    lad_put () {            # lad_put DIR: DIR's files onto the ladder disk, each in its directory, over what is there
        (cd $1 && find . -type f ! -name manifest.json ! -name index.json ! -name c4dos.dir \
            ! -name config.sys ! -name autoexec.bat | sed 's|^\./||') | while read -r f; do
            # a page disk's own directories are the ladder's too
            case "$f" in
                GRIN/*|BIN/*|GAMES/*) to=$f ;;
                *) d=$(lad_dir "$f"); to=${d:+$d/}$f ;;
            esac
            lj_put "$1/$f" $LJLAD "$to"
        done
    }
    lad_put $DISK
    lad_put $CLIMBSRC
    lad_put $LJDOS
    rm -f $LJLAD/C4IX/c4ix.c4r $LJLAD/C4KE/boot.cfg
    # The ladder's own files. Each name is unlinked first: what is there is
    # a hard link into one of the disks above, and a cp onto it writes
    # THROUGH the link, into that disk. That is how the climb floppy's
    # small C4IX tree (no /lib/libgui.c4s, no desktop, no games) took the
    # place of the shared disk's here, or the other way round, depending
    # on which was made last. C4IX's tree is the shared disk's, so the
    # C4IX the ladder builds comes up as ?system=c4ix does.
    rm -f $LJLAD/C4IX/c4ix.vfs.txt $LJLAD/BUILD/build.bat $LJLAD/ladder.txt
    cp $DISK/c4ix.vfs.txt $LJLAD/C4IX/c4ix.vfs.txt
    cp src/c4dos/fs/BUILD.BAT $LJLAD/BUILD/build.bat
    cp src/c4dos/fs/LADDER.TXT $LJLAD/ladder.txt
    # the climb floppy's RAM disk (32 MB: LADDER and the kernel's sources),
    # and the page's devices: HEAP.SYS gives each rung's memory back when
    # it returns to the one below, REDIR.SYS TAP is for grin's DOS Prompts.
    # No HOME=B:, as the page's C4DOS disk has: with a home drive, cpp and
    # c4cc would write the kernel to B: rather than the RAM disk C4KE is
    # seeded from.
    sed 's/SIZE=[0-9]*/SIZE=33554432/' src/c4dos/fs/CONFIG.SYS > $LJLAD/config.sys
    printf 'DEVICE=CONSOLE.SYS FLUSH\nDEVICE=DRIVES.SYS\nDEVICE=HEAP.SYS\nDEVICE=REDIR.SYS TAP\n' >> $LJLAD/config.sys
    list=$(echo $GAMES_ON_DISK | sed 's/ /, /g')
    {
        printf '@ECHO OFF\nECHO Welcome to C4DOS, the Build Ladder\nVER\n'
        printf 'PATH \\BUILD;\\GRIN;\\BIN\n'
        # C4KE and C4IX look for a file where they were started, as DOS
        # did before it had directories; APPEND says where else to look,
        # for them (the machine's DISK_DIRS) and for DOS
        printf 'REM C4KE and C4IX find their files, and the games, through APPEND\n'
        printf 'APPEND \\BUILD;\\C4KE;\\C4IX;%s\n' "$(games_dirs)"
        if [ -n "$list" ]; then printf 'ECHO The games, in \\GAMES: %s.\n' "$list"; fi
        # (C4DOS splits a line into at most 15 words, so ECHO says 14; and
        # no > in an ECHO: REDIR.SYS would take it for a redirection)
        printf 'ECHO GRIN starts the graphical shell.\n'
        printf 'ECHO Climb: BUILD, then RUN dosload.c4r c4ke.c4r boots the kernel it built.\n'
        printf 'ECHO TYPE LADDER.TXT says every step, up to C4IX.\n'
    } > $LJLAD/autoexec.bat
    listings $LJLAD
    disk_indexes $LJLAD
    echo "c4bb: ladder disk: $LJLAD ($(find $LJLAD -type f | wc -l) files, $(du -sh $LJLAD | cut -f1))"
else
    echo "c4bb: no ladder disk: it needs the climb floppy ('make c4bb-images' makes it) and $LJDOS" >&2
fi
