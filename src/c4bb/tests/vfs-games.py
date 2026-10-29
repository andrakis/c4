#!/usr/bin/env python3
# vfs-games.py IN OUT GAME... -- C4IX's manifest with the games that are
# on the disk: each one's own block (GAME/c4ix.vfs.txt) where #@games is,
# at its indent, and its programs' names, an ilink to each, where
# #@game-names is (/usr/games/bin, on the shell's PATH; games/README.md):
# every NAME.c4r its block puts in its directory, or else the game's own
# name. With none, both go.
import os, sys

src, out, games = sys.argv[1], sys.argv[2], sys.argv[3:]
lines = []
for line in open(src).read().split('\n'):
    mark = line.strip()
    pad = line[:len(line) - len(line.lstrip())]
    if mark == '#@games':
        for g in games:
            for l in open(os.path.join(g, 'c4ix.vfs.txt')).read().rstrip('\n').split('\n'):
                lines.append(pad + l if l else l)
    elif mark == '#@game-names':
        for g in games:
            n = os.path.basename(g.rstrip('/'))
            progs = [l.strip().split(':')[0][:-4] for l in open(os.path.join(g, 'c4ix.vfs.txt'))
                     if l.strip().split(':')[0].endswith('.c4r')] or [n]
            for p in progs:
                lines.append(f'{pad}{p}: ilink(/usr/games/{n}/{p}.c4r)')
    else:
        lines.append(line)
open(out, 'w').write('\n'.join(lines))
