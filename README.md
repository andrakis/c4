c4 - C in four functions
========================

An exercise in minimalism.

Try the following:

    gcc -o c4 c4.c     # Compile c4
    ./c4 hello.c       # Run a sample program
    ./c4 -s hello.c    # Generate assembly listing for hello.c

    ./c4 c4.c hello.c        # Run c4.c inside c4, then run hello.c
    ./c4 c4.c c4.c hello.c   # Run c4.c inside c4.c, inside c4, then run hello.c

    ./c4 sh.c                # Run the C4 shell

    ./c4 machine.c             # Run the c4 machine, runs asm/init.asm
    ./c4 -s c4.c > asm/c4.asm  # (Optional) regenerate c4.c assembly
    # Run c4.asm inside c4 machine
    ./c4 machine.c asm/c4.asm hello.c

    gcc -o machine machine.c          # Compile machine
    ./machine asm/c4.asm hello.c      # Run c4
    ./machine asm/c4.asm sh.c         # Run C4 shell
