# Runs a script under MinGW's python (tools/msys2/python3) writing text as
# Linux does: a Windows python turns '\n' into '\r\n' in every file it
# opens for writing as text, and on stdout. Generated sources and test
# output have to be the bytes the Linux build makes.
import builtins, io, os, runpy, sys

_open = io.open
def _lf_open(file, mode='r', buffering=-1, encoding=None, errors=None, newline=None, *a, **k):
    if 'b' not in mode and newline is None and any(c in mode for c in 'wax+'):
        newline = '\n'
    return _open(file, mode, buffering, encoding, errors, newline, *a, **k)
builtins.open = io.open = _lf_open
for f in (sys.stdout, sys.stderr):
    f.reconfigure(newline='\n')

sys.argv = sys.argv[1:]
if not sys.flags.isolated:
    sys.path[0] = os.path.dirname(os.path.abspath(sys.argv[0]))
runpy.run_path(sys.argv[0], run_name='__main__')
