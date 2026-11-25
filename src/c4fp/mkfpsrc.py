#!/usr/bin/env python3
# mkfpsrc.py -- softfp.c as the source c4lc splices into a unit that
# says float or double: c4lc-parse.lisp's p:fpsrc, between its markers.
#
#   python3 src/c4fp/mkfpsrc.py           rewrite p:fpsrc from softfp.c
#   python3 src/c4fp/mkfpsrc.py --check   fail if it is out of date
#
# The generator compiles a spliced helper only when something has called
# it by the time its turn comes, so callers must come before callees --
# the reverse of softfp.c's own order, which defines everything before
# using it. Each becomes static, as the long long helpers are: every unit
# carries its own, and two units' never meet at link time. Comments go;
# nothing else changes.
import re, sys, os

here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, 'softfp.c')
lisp = os.path.join(here, '..', 'c4sp', 'lisp', 'c4lc-parse.lisp')
BEGIN = ';; ---- p:fpsrc: generated from src/c4fp/softfp.c by src/c4fp/mkfpsrc.py ----\n'
END = ';; ---- end of p:fpsrc ----\n'

def functions(text):
    text = re.sub(r'//[^\n]*', '', text)
    out, cur, depth = [], [], 0
    for line in text.split('\n'):
        if not line.strip() and depth == 0:
            continue
        if depth == 0 and not cur and not re.match(r'[A-Za-z]', line):
            continue
        cur.append(line.rstrip())
        depth += line.count('{') - line.count('}')
        if depth == 0 and cur and '}' in line:
            out.append('\n'.join(l for l in cur if l.strip()))
            cur = []
    assert depth == 0 and not cur, 'unbalanced braces in softfp.c'
    return out

def block():
    fns = functions(open(src).read())
    body = '\n'.join('static ' + f for f in reversed(fns))
    assert '"' not in body and '\\' not in body, 'softfp.c code must have no quotes or backslashes'
    return (BEGIN +
            ';; What c4lc-gen calls for float and double (docs/c4-float.md): spliced\n'
            ';; after a unit that says float or double, callers first, all static.\n'
            '(define p:fpsrc "\n' + body + '\n")\n' + END)

def main():
    text = open(lisp).read()
    i, j = text.find(BEGIN), text.find(END)
    if i < 0 or j < 0:
        sys.exit('mkfpsrc: no p:fpsrc markers in ' + lisp)
    new = text[:i] + block() + text[j + len(END):]
    if '--check' in sys.argv:
        if new != text:
            sys.exit('mkfpsrc: p:fpsrc is out of date with softfp.c -- run python3 src/c4fp/mkfpsrc.py')
        print('mkfpsrc: p:fpsrc matches softfp.c')
        return
    if new != text:
        open(lisp, 'w').write(new)
        print('mkfpsrc: p:fpsrc rewritten')
    else:
        print('mkfpsrc: p:fpsrc already up to date')

main()
