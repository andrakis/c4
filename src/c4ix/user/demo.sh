# C4IX X4 demo script, run by c4ix-sh.
# Everything below is done by the shell through syscalls alone:
# spawn, dup2, pipe, open, close, wait.

echo hello from c4ix-sh

# output redirection into a RAM file, then read it back
echo one two three > /ram/words
cat /ram/words

# a pipeline, and a pipeline whose tail is redirected
cat /ram/words | wc
cat /ram/words | wc > /ram/count
cat /ram/count

# input redirection
wc < /ram/words

# a three-stage pipeline
cat /ram/words | cat | wc

# operators need no spaces around them
cat /ram/words|wc

# a background job: started, listed, then reaped
echo backgrounded > /ram/bg &
jobs
wait
cat /ram/bg

# builtins that report rather than pretend
cd /somewhere

exit 3
