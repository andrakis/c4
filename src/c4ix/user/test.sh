# C4IX test suite, run by c4ix-sh.
#
# The C4KE suite under src/tests/ cannot be run here: those images are
# built against u0.h and C4KE's custom opcodes, so they are binaries
# for a different operating system. What is portable is the COVERAGE,
# so the suite is re-expressed against C4IX's own interfaces -- and it
# is driven by the shell, which means every line below is also a test
# of spawn, wait, pipe, dup2 and the fd table.
#
# Each check prints its own name and result, so the whole file is a
# transcript that either matches the pin exactly or does not.

echo test: files
echo alpha beta > /tmp/t1
cat /tmp/t1
wc < /tmp/t1

echo test: pipes
cat /tmp/t1 | wc
cat /tmp/t1 | cat | cat | wc

echo test: redirection
cat /tmp/t1 | wc > /tmp/t2
cat /tmp/t2

echo test: overwrite truncates
echo x > /tmp/t2
cat /tmp/t2
wc < /tmp/t2

echo test: directories
mkdir /d
mkdir /d/sub
echo nested > /d/sub/f
ls /
ls /d
cd /d/sub
pwd
cat f
cd ..
pwd
cd /
cat /d/sub/f

echo test: process table
ps -s

echo test: exit status of a pipeline tail
cat /tmp/nonexistent
echo after a failure the shell keeps going

echo test: done
exit 0
