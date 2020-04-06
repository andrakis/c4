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
    ./c4 -s c4.c > c4.asm      # Generate c4.c assembly
    # Run c4.asm inside c4 machine
    ./c4 machine.c c4.asm hello.c

    gcc -o machine machine.c   # Compile machine
    ./machine c4.asm hello.c   # Run c4
    ./machine c4.asm sh.c      # Run C4 shell

What is the C4 Machine?
-----------------------

The C4 Machine is an experiment as to what is possible to do on C4. It is designed to run on an updated version of C4, but that version of C4 is fully compatible with the original C4. That is to say, no recompilation of C4 is neccesary.

It features an assembler which will run code output from C4's `-s` parameter, including C4 itself. The assembler can support more opcodes than C4, allowing additional functionality to be implemented.

The C4 Machine consists of several files:

  - machine.c
  
    The C4 Machine. Contains an assembler and an updated virtual machine section.
    
  - c4-machine/fs.c
  
    The C4 filesystem. Can read (and write, if compiled seperately) image files of a simple nature.
    
   - c4-machine/cc.0.c
   
     The basic C4 C compiler, with all other options stripped out. Is able to produce assembly source listings, but contains no support for advanced opcodes.
    
     This is intended to be a bootstrap compiler for the more advanced version below.
    
   - c4-machine/cc.c
   
     The more advanced C4 C compiler. Similar to the above bootstrap compiler, but uses the extended opcodes to implement new features (such as grabbing opcode values from the hosting platform.)
    
The eventual goal is to get a simple operating system running on the C4 Machine, as well as enough tools (shell, compiler, linker, etc) to support local development.
