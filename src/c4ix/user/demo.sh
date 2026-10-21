# C4IX X4 demo script, run by c4ix-sh.
# Everything below is done by the shell through syscalls alone:
# spawn, dup2, pipe, open, close, wait.

echo hello from c4ix-sh

# output redirection into a RAM file, then read it back
echo one two three > /tmp/words
cat /tmp/words

# a pipeline, and a pipeline whose tail is redirected
cat /tmp/words | wc
cat /tmp/words | wc > /tmp/count
cat /tmp/count

# input redirection
wc < /tmp/words

# a three-stage pipeline
cat /tmp/words | cat | wc

# operators need no spaces around them
cat /tmp/words|wc

# a background job: started, listed, then reaped
echo backgrounded > /tmp/bg &
jobs
wait
cat /tmp/bg

# builtins that report rather than pretend
cd /somewhere

exit 3
