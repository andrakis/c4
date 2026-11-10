#!/usr/bin/env python3
# vfs-games.py IN OUT GAME... -- C4IX's manifest with the games that are
# on the disk: each one's own block (GAME/c4ix.vfs.txt) where #@games is,
# at its indent, and its programs' names, an ilink to each, where
# #@game-names is (/usr/games/bin, on the shell's PATH; games/README.md):
# every NAME.c4r its block puts in its directory, or else the game's own
# name. With none, both go.
#
# And what the desktop's Go menu lists: every "#@app LINE" in a block is a
# line of /etc/desktop.apps (games/README.md). They are written to
# c4ix-desktop.apps beside OUT, on the disk, and #@desktop-apps becomes its
# entry. With none, that goes too.
import os, sys

src, out, games = sys.argv[1], sys.argv[2], sys.argv[3:]
def block(g):
    return open(os.path.join(g, 'c4ix.vfs.txt')).read().rstrip('\n').split('\n')
apps = [l[len('#@app '):] for g in games for l in block(g) if l.startswith('#@app ')]
apps_file = os.path.join(os.path.dirname(out), 'c4ix-desktop.apps')
if apps:
    open(apps_file, 'w').write(''.join(a + '\n' for a in apps))
elif os.path.exists(apps_file):
    os.remove(apps_file)
lines = []
for line in open(src).read().split('\n'):
    mark = line.strip()
    pad = line[:len(line) - len(line.lstrip())]
    if mark == '#@games':
        for g in games:
            for l in block(g):
                if not l.startswith('#@'):
                    lines.append(pad + l if l else l)
    elif mark == '#@game-names':
        # one directory: a name two games both give a program would be the
        # second's, and the first's unreachable by name (the Ace of Penguins
        # and the BSD games each had a canfield)
        owner = {}
        for g in games:
            n = os.path.basename(g.rstrip('/'))
            progs = [l.strip().split(':')[0][:-4] for l in block(g)
                     if not l.startswith('#') and l.strip().split(':')[0].endswith('.c4r')] or [n]
            for p in progs:
                if p in owner:
                    sys.exit(f'vfs-games: {owner[p]} and {n} both have a program named {p}')
                owner[p] = n
                lines.append(f'{pad}{p}: ilink(/usr/games/{n}/{p}.c4r)')
    elif mark == '#@desktop-apps':
        if apps:
            lines.append(f'{pad}etc/:')
            lines.append(f'{pad}\tdesktop.apps: c4ix-desktop.apps')
    else:
        lines.append(line)
open(out, 'w').write('\n'.join(lines))
