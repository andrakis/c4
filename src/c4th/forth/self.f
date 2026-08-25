\ c4th self.f -- a Forth compiler, in the Forth it compiles.
\
\ This is B5d.5, the fixed point. It reads Forth source and writes a
\ .c4r image to standard output, and the source it is meant to read is
\ this file. Three generations settle it:
\
\   c4th + self.f   compiling self.f  ->  gen1.c4r
\   gen1.c4r        compiling self.f  ->  gen2.bin
\   gen2.bin        compiling self.f  ->  gen3.bin
\
\ and gen1 == gen2 == gen3. The first equality is the interesting one:
\ the compiler running on c4th's threaded engine -- the engine that
\ passes the Forth-2012 CORE suite -- and the same compiler running as
\ native C4 code emit the same bytes.
\
\ WHY NOT native.f. native.f's input is c4th's threaded code, not text.
\ For a generated image to compile native.f it would first have to BE
\ c4th -- outer interpreter, threaded compiler, dictionary, inner
\ interpreter -- which is the whole C kernel ported before the rung is
\ even reached. A fixed point needs a compiler written in the language it
\ compiles, and that is what this is. native.f stays the fast backend.
\
\ THE CODE MODEL is subroutine threading over a software data stack --
\ strategy (a) of docs/c4th-design.md §8. Every Forth word is one
\ JSR to a helper; the helpers are emitted by this file into the image
\ ahead of everything else, most of them from four templates. It is
\ slower than native.f's strategy (b) and much simpler, which is the
\ right trade for the one program that has to compile itself.
\
\ Every opcode emitted is <= EXIT, so a generated image runs on plain c4
\ as well as on c4m, c4mp and oisc4.
\
\ THE DIALECT is whatever this file's own source uses, and no more --
\ that is the closure condition, and the compiler enforces it by
\ refusing a word it does not know.

\ -- C4 opcodes ---------------------------------------------------------

 0 CONSTANT oLEA    1 CONSTANT oIMM    2 CONSTANT oJMP    3 CONSTANT oJSR
 4 CONSTANT oBZ     5 CONSTANT oBNZ    6 CONSTANT oENT    7 CONSTANT oADJ
 8 CONSTANT oLEV    9 CONSTANT oLI    10 CONSTANT oLC    11 CONSTANT oSI
12 CONSTANT oSC    13 CONSTANT oPSH   14 CONSTANT oOR    15 CONSTANT oXOR
16 CONSTANT oAND   17 CONSTANT oEQ    18 CONSTANT oNE    19 CONSTANT oLT
20 CONSTANT oGT    21 CONSTANT oLE    22 CONSTANT oGE    23 CONSTANT oSHL
24 CONSTANT oSHR   25 CONSTANT oADD   26 CONSTANT oSUB   27 CONSTANT oMUL
28 CONSTANT oDIV   29 CONSTANT oMOD   30 CONSTANT oOPEN  31 CONSTANT oREAD
32 CONSTANT oCLOS  33 CONSTANT oPRTF  34 CONSTANT oMALC  38 CONSTANT oEXIT

-1 CONSTANT LTCODE
-2 CONSTANT LTDATA

\ -- sizes --------------------------------------------------------------

262144 CONSTANT CMAX            \ image code words
131072 CONSTANT DMAX            \ image data bytes
 98304 CONSTANT PMAX            \ patches
1048576 CONSTANT SMAX           \ source bytes
131072 CONSTANT NMAX            \ dictionary name bytes
  4096 CONSTANT WMAX            \ dictionary entries
     5 CONSTANT WSZ             \ cells per entry

\ -- the image being built ----------------------------------------------

VARIABLE CBUF   VARIABLE CN     \ code words, next free index (1-based)
VARIABLE DBUF   VARIABLE DN     \ data bytes, next free offset
VARIABLE PBUF   VARIABLE PN     \ patches: type, addr, value
VARIABLE ENTRYW                 \ code index of the entry

\ -- the source ---------------------------------------------------------

VARIABLE SBUF   VARIABLE SLEN   VARIABLE SPOS
VARIABLE SRCP                   \ nul-terminated path, or 0

\ -- the dictionary -----------------------------------------------------

VARIABLE NBLOB  VARIABLE NBN
VARIABLE DICT   VARIABLE DICTN

\ -- compiler state -----------------------------------------------------

VARIABLE CSTATE                 \ 0 interpreting, 1 compiling
VARIABLE CURW                   \ code index of the definition in hand
VARIABLE LOOPD                  \ how many DO loops are open
VARIABLE ERRN                   \ nonzero once something went wrong

\ the compile-time value stack, for  1024 CELLS CONSTANT FOO
CREATE CTS 32 CELLS ALLOT   VARIABLE CTSP
\ the control-flow stack: marker, address
CREATE CSTK 128 CELLS ALLOT VARIABLE CSP

\ helper addresses, filled in by BUILD-RUNTIME
VARIABLE hPUSH  VARIABLE hPOPA
VARIABLE FMTO                   \ data offset of "%c"
VARIABLE TMPO   VARIABLE TMP2O  \ scratch cells in the image
VARIABLE DSPO   VARIABLE RSPO   VARIABLE LSPO
VARIABLE DSBO                   \ data offset of the data stack's base

\ argv, as the entry stub leaves them
VARIABLE ARGC   VARIABLE ARGV

\ -- the few things written in Forth rather than emitted -----------------
\ MOVE and FILL are not primitives here: plain c4 has no MCPY, and a
\ forward byte loop is four lines. Naming them BMOVE/BFILL keeps them
\ clear of c4th's own, which this file also runs on.
\
\ Nothing below uses >R, R> or R@, and nothing recurses. Both are
\ deliberate. c4th's DO/LOOP keeps its parameters on the return stack
\ and this compiler's keeps them on a stack of their own, so an R@
\ inside a loop would read one thing hosted and another compiled -- and
\ this file has to mean the same thing both ways. Recursion is avoided
\ because a word is not visible inside its own definition, so it would
\ need RECURSE, and RECURSE is a directive this compiler would then owe.

: CR   10 EMIT ;
: TYPE ( a n -- )  0 ?DO DUP C@ EMIT 1+ LOOP DROP ;
: BMOVE ( src dst n -- )
   0 ?DO OVER C@ OVER C! 1+ SWAP 1+ SWAP LOOP 2DROP ;
: BFILL ( a n c -- )
   SWAP 0 ?DO 2DUP SWAP C! SWAP 1+ SWAP LOOP 2DROP ;
: SCMP ( a1 a2 n -- f )         \ true if the n bytes agree
   0 ?DO OVER C@ OVER C@ <> IF 2DROP 0 UNLOOP EXIT THEN 1+ SWAP 1+ SWAP LOOP
   2DROP -1 ;

\ Diagnostics. A compiler that stops without saying why is no use.
CREATE UDBUF 24 ALLOT   VARIABLE UDP
: UDOT ( u -- )
   UDBUF 24 + UDP !
   BEGIN
      -1 UDP +!
      DUP 10 MOD 48 + UDP @ C!
      10 /
      DUP 0=
   UNTIL DROP
   UDP @ BEGIN DUP UDBUF 24 + < WHILE DUP C@ EMIT 1+ REPEAT DROP ;

: DIE ( -- )  1 HALT ;
: BAD ( a n -- )
   S" self: " TYPE TYPE S"  ?" TYPE CR DIE ;

\ -- building the image --------------------------------------------------
\ Code word 0 is never an instruction: the .c4r code stream is 1-based,
\ because c4cc emits through *++e. B5d found that the hard way -- an
\ image c4m ran perfectly and every tool that WALKS the stream
\ mis-decoded from the first word.

VARIABLE TQ  VARIABLE PQ                \ scratch for the emitters below

: CHERE ( -- n )  CN @ ;
: K, ( w -- )
   CN @ CMAX < 0= IF S" code overflow" BAD THEN
   CN @ CELLS CBUF @ + !  1 CN +! ;
: DALLOT ( n -- off )
   DN @ 1 CELLS 1- + 1 CELLS 1- INVERT AND DN !
   DN @ SWAP DN +!
   DN @ DMAX < 0= IF S" data overflow" BAD THEN ;
: DB! ( c off -- )  DBUF @ + C! ;
: PAT, ( type addr value -- )
   PN @ PMAX < 0= IF S" patch overflow" BAD THEN
   PN @ 3 * CELLS PBUF @ + PQ !
   PQ @ 2 CELLS + !                      \ value
   PQ @ 1 CELLS + !                      \ addr
   PQ @ !                                \ type
   1 PN +! ;

\ -- the assembler -------------------------------------------------------
\ An operand is a reference in exactly two cases -- a branch or call
\ target, and an IMM of an address in the image's data -- and in both the
\ reference only has to be RECORDED, because load-c4r.c applies a patch
\ as *(code+addr) = (int)(code+val) or (int)(data+val). Recording every
\ intra-image pointer while building into a buffer IS the patch table.

: OP,  ( op -- )     K, ;
: OP2, ( n op -- )   K, K, ;
: IMMD, ( off -- )                      \ IMM of a data address
   TQ !  oIMM OP,  LTDATA CN @ TQ @ PAT,  TQ @ K, ;
: BJ,  ( op target -- )                 \ a jump or call to a known place
   TQ !  OP,  LTCODE CN @ TQ @ PAT,  TQ @ K, ;
: JSRC, ( target -- )  oJSR SWAP BJ, ;
: FMARK, ( op -- p )                    \ a forward branch, unresolved
\ The patch is recorded NOW, with a value of zero, and filled in when the
\ branch is resolved -- rather than recorded at resolve time, which is
\ the obvious way and puts the patch table out of order. c4r.lisp walks
\ the instruction stream and the patch list together and says so:
\ "patch list out of sync with instruction stream". So what a forward
\ branch carries around is its PATCH index, not its code address; the
\ code address is in the patch.
   OP,  LTCODE CN @ 0 PAT,  0 K,  PN @ 1- ;
: FRES ( p -- )                         \ resolve it to here
   3 * CELLS PBUF @ + TQ !
   CN @  TQ @ 2 CELLS + !
   CN @  TQ @ 1 CELLS + @ CELLS CBUF @ + ! ;

\ Reaching the three software stacks. Every one of them is a cell in the
\ image's data holding a pointer to an array also in the image's data:
\ the C4 stack cannot be the data stack here, because a JSR pushes its
\ return address onto it, and subroutine threading is nothing but JSRs.

: OFF, ( k -- )  ?DUP IF oPSH OP, oIMM OP2, oADD OP, THEN ;
: DSP@,          DSPO @ IMMD, oLI OP, ;
: DSPA, ( k -- ) DSP@, OFF, ;
: DSPC, ( k -- ) DSPA, oLI OP, ;
: DSPADD, ( k -- ) DSPO @ IMMD, oPSH OP, DSPA, oSI OP, ;
: RSP@,          RSPO @ IMMD, oLI OP, ;
: RSPA, ( k -- ) RSP@, OFF, ;
: RSPC, ( k -- ) RSPA, oLI OP, ;
: RSPADD, ( k -- ) RSPO @ IMMD, oPSH OP, RSPA, oSI OP, ;
: LSP@,          LSPO @ IMMD, oLI OP, ;
: LSPA, ( k -- ) LSP@, OFF, ;
: LSPC, ( k -- ) LSPA, oLI OP, ;
: LSPADD, ( k -- ) LSPO @ IMMD, oPSH OP, LSPA, oSI OP, ;
: MOVC, ( src dst -- )  DSPA, oPSH OP,  DSPC, oSI OP, ;
: TMP!, ( k -- )  TMPO @ IMMD, oPSH OP, DSPC, oSI OP, ;
: TMP@,           TMPO @ IMMD, oLI OP, ;
: TM2!, ( k -- )  TMP2O @ IMMD, oPSH OP, DSPC, oSI OP, ;
: TM2@,           TMP2O @ IMMD, oLI OP, ;

\ -- the dictionary ------------------------------------------------------
\ Five cells: where the name is, how long, what kind of thing, its value,
\ and whether it can be folded at compile time. Kinds: 0 code (a
\ primitive's helper or a colon word -- they compile identically, which
\ is the whole appeal of subroutine threading), 2 constant, 3 data, 4
\ compiler directive.

VARIABLE NQ  VARIABLE EQ  VARIABLE PV  VARIABLE FV
VARIABLE FA  VARIABLE FN

: W[] ( i -- a )  WSZ * CELLS DICT @ + ;
: NSTORE ( a n -- off len )
   NQ !  NBLOB @ NBN @ +  NQ @ BMOVE
   NBN @ NQ @  NQ @ NBN +! ;
: DEFE ( off len kind val fold -- )
   DICTN @ WMAX < 0= IF S" dictionary full" BAD THEN
   DICTN @ W[] EQ !
   EQ @ 4 CELLS + !
   EQ @ 3 CELLS + !
   EQ @ 2 CELLS + !
   EQ @ 1 CELLS + !
   EQ @ !
   1 DICTN +! ;
: FINDW ( a n -- i )                    \ -1 if it is not there
   FN ! FA !
   DICTN @ 0 ?DO
      DICTN @ 1- I -
      DUP W[] 1 CELLS + @ FN @ = IF
         DUP W[] @ NBLOB @ + FA @ FN @ SCMP IF UNLOOP EXIT THEN
      THEN
      DROP
   LOOP -1 ;
: DEFP  ( addr a n -- )       NSTORE ROT PV ! 0 PV @ 0 DEFE ;
: DEFPF ( addr a n fold -- )  FV ! NSTORE ROT PV ! 0 PV @ FV @ DEFE ;
: DEFD  ( id a n -- )         NSTORE ROT PV ! 4 PV @ 0 DEFE ;

\ -- the runtime library -------------------------------------------------
\ Every Forth word compiles to one JSR into this. The helpers take their
\ arguments off the software data stack and put results back, so nothing
\ is passed the C4 way and nothing has to be adjusted afterwards -- with
\ one exception, the literal pusher, which takes its value as a real C4
\ argument because that is cheaper at the call site than four
\ instructions of inline store.
\
\ ENT 0 / LEV around each one is not decoration: LEV restores sp from bp,
\ which is what makes it safe for a helper to leave the C4 stack
\ unbalanced, and the accumulator survives both -- which is how POPA
\ hands a flag back to the BZ that follows its call.

1024 CONSTANT DSMAX
 256 CONSTANT RSMAX
 192 CONSTANT LSMAX

VARIABLE RSBO   VARIABLE LSBO
VARIABLE hDO    VARIABLE hQDO   VARIABLE hLOOP
VARIABLE hPLOOP VARIABLE hUNLP
VARIABLE HA     VARIABLE HQ     VARIABLE HK

: H0 ( -- )       CN @ HA !  0 oENT OP2, ;
: H1 ( -- addr )  oLEV OP,  HA @ ;

\ Four templates cover forty of the fifty-odd words. C4's ALU ops are
\ a = *sp++ OP a, which is a Forth stack machine already -- the only
\ mismatch is that C4's comparisons yield 0/1 and Forth's yield 0/-1,
\ which is one SUB from zero.
: HBIN ( op -- addr )
   HQ ! H0
   -16 DSPA, oPSH OP,
   -16 DSPC, oPSH OP,
    -8 DSPC,
   HQ @ OP,
   oSI OP,
   -8 DSPADD,
   H1 ;
: HCMP ( op -- addr )
   HQ ! H0
   -16 DSPA, oPSH OP,
   0 oIMM OP2, oPSH OP,
   -16 DSPC, oPSH OP,
    -8 DSPC,
   HQ @ OP,
   oSUB OP,
   oSI OP,
   -8 DSPADD,
   H1 ;
: HUNI ( k op -- addr )                 \ x OP k, in place
   HQ ! HK ! H0
   -8 DSPA, oPSH OP,
   -8 DSPC,
   oPSH OP, HK @ oIMM OP2, HQ @ OP,
   oSI OP,
   H1 ;
: HUZ ( op -- addr )                    \ x OP 0, as a Forth flag
   HQ ! H0
   -8 DSPA, oPSH OP,
   0 oIMM OP2, oPSH OP,
   -8 DSPC, oPSH OP,
   0 oIMM OP2,
   HQ @ OP,
   oSUB OP,
   oSI OP,
   H1 ;
: HNOP ( -- addr )  H0 H1 ;

: HPUSH ( -- addr )                     \ ( -- ) with the value as a C4 arg
   H0
   DSP@, oPSH OP,
   2 oLEA OP2, oLI OP,
   oSI OP,
   8 DSPADD,
   H1 ;
: HPOPA ( -- addr )                     \ pop into the accumulator
   H0
   -8 DSPADD,
   DSP@, oLI OP,
   H1 ;

: HDUP   H0  -8 0 MOVC,  8 DSPADD,  H1 ;
: HDROP  H0  -8 DSPADD,  H1 ;
: H2DROP H0 -16 DSPADD,  H1 ;
: HSWAP  H0  -8 TMP!,  -16 -8 MOVC,  -16 DSPA, oPSH OP, TMP@, oSI OP,  H1 ;
: HOVER  H0 -16 0 MOVC,  8 DSPADD,  H1 ;
: HNIP   H0  -8 -16 MOVC, -8 DSPADD,  H1 ;
: HTUCK  H0  -8 TMP!,  0 DSPA, oPSH OP, TMP@, oSI OP,
             -16 -8 MOVC,
             -16 DSPA, oPSH OP, TMP@, oSI OP,
             8 DSPADD,  H1 ;
: HROT   H0 -24 TMP!,  -16 -24 MOVC,  -8 -16 MOVC,
             -8 DSPA, oPSH OP, TMP@, oSI OP,  H1 ;
: H2DUP  H0 -16 0 MOVC,  -8 8 MOVC,  16 DSPADD,  H1 ;
: HQDUP  H0  -8 DSPC,  oBZ FMARK,  -8 0 MOVC,  8 DSPADD,  FRES  H1 ;
: HDEPTH H0  0 DSPA, oPSH OP,
             DSP@, oPSH OP, DSBO @ IMMD, oSUB OP,
             oPSH OP, 1 CELLS oIMM OP2, oDIV OP,
             oSI OP,  8 DSPADD,  H1 ;

: HTOR   H0  RSP@, oPSH OP, -8 DSPC, oSI OP,  1 CELLS RSPADD,  -8 DSPADD,  H1 ;
: HRFROM H0  1 CELLS NEGATE RSPADD,
             0 DSPA, oPSH OP, RSP@, oLI OP, oSI OP,  8 DSPADD,  H1 ;
: HRAT   H0  0 DSPA, oPSH OP, 1 CELLS NEGATE RSPC, oSI OP,  8 DSPADD,  H1 ;

: HFETCH  H0 -8 DSPA, oPSH OP, -8 DSPC, oLI OP, oSI OP, H1 ;
: HCFETCH H0 -8 DSPA, oPSH OP, -8 DSPC, oLC OP, oSI OP, H1 ;
: HSTORE  H0 -8 DSPC, oPSH OP, -16 DSPC, oSI OP, -16 DSPADD, H1 ;
: HCSTORE H0 -8 DSPC, oPSH OP, -16 DSPC, oSC OP, -16 DSPADD, H1 ;
: HPLUSST H0 -8 DSPC, oPSH OP,
             -8 DSPC, oLI OP, oPSH OP, -16 DSPC, oADD OP,
             oSI OP, -16 DSPADD, H1 ;

: HABS   H0 -8 DSPC, oPSH OP, 0 oIMM OP2, oLT OP,
            oBZ FMARK,
            -8 DSPA, oPSH OP, 0 oIMM OP2, oPSH OP, -8 DSPC, oSUB OP, oSI OP,
            FRES  H1 ;
: HMIN   H0 -16 DSPC, oPSH OP, -8 DSPC, oGT OP,
            oBZ FMARK,  -8 -16 MOVC,  FRES
            -8 DSPADD,  H1 ;
: HMAX   H0 -16 DSPC, oPSH OP, -8 DSPC, oLT OP,
            oBZ FMARK,  -8 -16 MOVC,  FRES
            -8 DSPADD,  H1 ;
: MASKV ( -- n )  1 1 CELLS 8 * 1- LSHIFT ;      \ the sign bit
: HRSHIFT H0                                     \ logical, where C4's SHR is not
            -16 DSPA, oPSH OP,
            -8 DSPC,
            oBZ FMARK,
               -16 DSPC, oPSH OP, 1 oIMM OP2, oSHR OP,
               oPSH OP, MASKV INVERT oIMM OP2, oAND OP,
               oPSH OP, -8 DSPC, oPSH OP, 1 oIMM OP2, oSUB OP, oSHR OP,
               oJMP FMARK,
            SWAP FRES
               -16 DSPC,
            FRES
            oSI OP, -8 DSPADD, H1 ;
: HULT   H0 -16 DSPA, oPSH OP,
            0 oIMM OP2, oPSH OP,
            -16 DSPC, oPSH OP, MASKV oIMM OP2, oXOR OP, oPSH OP,
             -8 DSPC, oPSH OP, MASKV oIMM OP2, oXOR OP,
            oLT OP, oSUB OP, oSI OP, -8 DSPADD, H1 ;
: HDIVMOD H0 -16 TMP!,  -8 TM2!,
            -16 DSPA, oPSH OP, TMP@, oPSH OP, TM2@, oMOD OP, oSI OP,
             -8 DSPA, oPSH OP, TMP@, oPSH OP, TM2@, oDIV OP, oSI OP,
            H1 ;

: HEMIT  H0 FMTO @ IMMD, oPSH OP,  -8 DSPC, oPSH OP,
            oPRTF OP, 2 oADJ OP2,  -8 DSPADD, H1 ;
: HHALT  H0 -8 DSPC, oPSH OP, oEXIT OP, 1 oADJ OP2, H1 ;
: HALLOC H0 -8 DSPA, oPSH OP,
            -8 DSPC, oPSH OP, oMALC OP, 1 oADJ OP2,
            oSI OP, H1 ;
: HOPEN  H0 -8 DSPA, oPSH OP,
            -8 DSPC, oPSH OP, 0 oIMM OP2, oPSH OP, oOPEN OP, 2 oADJ OP2,
            oSI OP, H1 ;
: HREAD  H0 -24 DSPA, oPSH OP,
            -24 DSPC, oPSH OP, -16 DSPC, oPSH OP, -8 DSPC, oPSH OP,
            oREAD OP, 3 oADJ OP2,
            oSI OP, -16 DSPADD, H1 ;
: HCLOSE H0 -8 DSPC, oPSH OP, oCLOS OP, 1 oADJ OP2, -8 DSPADD, H1 ;

\ Counted loops keep limit and index on a stack of their own, not on the
\ return stack, because the return stack here is the C4 stack and a JSR
\ is already using it.
: HDOP   H0 LSP@, oPSH OP, -16 DSPC, oSI OP,
            1 CELLS LSPA, oPSH OP, -8 DSPC, oSI OP,
            2 CELLS LSPADD,  -16 DSPADD,  H1 ;
: HQDOP  H0 -16 TMP!,  -8 TM2!,
            LSP@, oPSH OP, TMP@, oSI OP,
            1 CELLS LSPA, oPSH OP, TM2@, oSI OP,
            2 CELLS LSPADD,  -16 DSPADD,
            TMP@, oPSH OP, TM2@, oNE OP,  H1 ;
: HI     H0 0 DSPA, oPSH OP, 1 CELLS NEGATE LSPC, oSI OP, 8 DSPADD, H1 ;
: HJ     H0 0 DSPA, oPSH OP, 3 CELLS NEGATE LSPC, oSI OP, 8 DSPADD, H1 ;
: HLOOPP H0 1 CELLS NEGATE LSPA, oPSH OP,
            1 CELLS NEGATE LSPC, oPSH OP, 1 oIMM OP2, oADD OP, oSI OP,
            1 CELLS NEGATE LSPC, oPSH OP, 2 CELLS NEGATE LSPC, oNE OP, H1 ;
: HPLOOP H0 -8 TM2!,  -8 DSPADD,
            TMPO @ IMMD, oPSH OP,
               1 CELLS NEGATE LSPC, oPSH OP, 2 CELLS NEGATE LSPC, oSUB OP,
            oSI OP,
            1 CELLS NEGATE LSPA, oPSH OP,
               1 CELLS NEGATE LSPC, oPSH OP, TM2@, oADD OP,
            oSI OP,
            TMP@, oPSH OP,
               1 CELLS NEGATE LSPC, oPSH OP, 2 CELLS NEGATE LSPC, oSUB OP,
            oXOR OP,
            oPSH OP, 0 oIMM OP2, oGE OP,  H1 ;
: HUNLOOP H0 2 CELLS NEGATE LSPADD, H1 ;

\ -- laying out the runtime's data, and registering its words ------------

: RTDATA
   1 CELLS DALLOT DSPO !
   1 CELLS DALLOT RSPO !
   1 CELLS DALLOT LSPO !
   1 CELLS DALLOT TMPO !
   1 CELLS DALLOT TMP2O !
   3 DALLOT FMTO !
   37 FMTO @ DB!   99 FMTO @ 1+ DB!   0 FMTO @ 2 + DB!
   DSMAX CELLS DALLOT DSBO !
   RSMAX CELLS DALLOT RSBO !
   LSMAX CELLS DALLOT LSBO ! ;

: RT-STACK
   HPUSH hPUSH !   HPOPA hPOPA !
   HDUP   S" DUP"   DEFP
   HDROP  S" DROP"  DEFP
   HSWAP  S" SWAP"  DEFP
   HOVER  S" OVER"  DEFP
   HROT   S" ROT"   DEFP
   HNIP   S" NIP"   DEFP
   HTUCK  S" TUCK"  DEFP
   HQDUP  S" ?DUP"  DEFP
   H2DUP  S" 2DUP"  DEFP
   H2DROP S" 2DROP" DEFP
   HDEPTH S" DEPTH" DEFP
   HTOR   S" >R"    DEFP
   HRFROM S" R>"    DEFP
   HRAT   S" R@"    DEFP ;

: RT-MATH
   oADD HBIN S" +"      2 DEFPF
   oSUB HBIN S" -"      3 DEFPF
   oMUL HBIN S" *"      1 DEFPF
   oDIV HBIN S" /"        DEFP
   oMOD HBIN S" MOD"      DEFP
   oAND HBIN S" AND"      DEFP
   oOR  HBIN S" OR"       DEFP
   oXOR HBIN S" XOR"      DEFP
   oSHL HBIN S" LSHIFT"   DEFP
   HRSHIFT  S" RSHIFT"    DEFP
   HDIVMOD  S" /MOD"      DEFP
   HABS     S" ABS"       DEFP
   HMIN     S" MIN"       DEFP
   HMAX     S" MAX"       DEFP
   1 oADD HUNI S" 1+"     8 DEFPF
   1 oSUB HUNI S" 1-"     9 DEFPF
   1 oSHL HUNI S" 2*"    10 DEFPF
   1 oSHR HUNI S" 2/"       DEFP
   -1 oMUL HUNI S" NEGATE"  DEFP
   -1 oXOR HUNI S" INVERT"  DEFP
   1 CELLS oMUL HUNI S" CELLS"  4 DEFPF
   1 CELLS oADD HUNI S" CELL+"  5 DEFPF
   HNOP        S" CHARS"        6 DEFPF
   1 oADD HUNI S" CHAR+"        7 DEFPF ;

: RT-COMPARE
   oEQ HCMP S" ="   DEFP
   oNE HCMP S" <>"  DEFP
   oLT HCMP S" <"   DEFP
   oGT HCMP S" >"   DEFP
   oLE HCMP S" <="  DEFP
   oGE HCMP S" >="  DEFP
   HULT     S" U<"  DEFP
   oEQ HUZ S" 0="   DEFP
   oNE HUZ S" 0<>"  DEFP
   oLT HUZ S" 0<"   DEFP
   oGT HUZ S" 0>"   DEFP ;

: RT-REST
   HFETCH  S" @"        DEFP
   HSTORE  S" !"        DEFP
   HCFETCH S" C@"       DEFP
   HCSTORE S" C!"       DEFP
   HPLUSST S" +!"       DEFP
   HEMIT   S" EMIT"     DEFP
   HHALT   S" HALT"     DEFP
   HALLOC  S" ALLOCATE" DEFP
   HOPEN   S" OPENF"    DEFP
   HREAD   S" READF"    DEFP
   HCLOSE  S" CLOSEF"   DEFP
   HDOP   hDO !   HQDOP hQDO !  HLOOPP hLOOP !
   HPLOOP hPLOOP ! HUNLOOP hUNLP !
   HI S" I" DEFP
   HJ S" J" DEFP
   hUNLP @ S" UNLOOP" DEFP ;

CREATE NMBUF 8 ALLOT
: NM1 ( c -- a n )      NMBUF C! NMBUF 1 ;
: NM2 ( c1 c2 -- a n )  NMBUF 1+ C! NMBUF C! NMBUF 2 ;

: RT-DIRECTIVES
    1 S" :"        DEFD    2 S" ;"        DEFD
    3 S" IF"       DEFD    4 S" ELSE"     DEFD    5 S" THEN"  DEFD
    6 S" BEGIN"    DEFD    7 S" UNTIL"    DEFD    8 S" WHILE" DEFD
    9 S" REPEAT"   DEFD   10 S" AGAIN"    DEFD
   11 S" DO"       DEFD   12 S" ?DO"      DEFD
   13 S" LOOP"     DEFD   14 S" +LOOP"    DEFD
   15 S" EXIT"     DEFD
   17 S" VARIABLE" DEFD   18 S" CONSTANT" DEFD
   19 S" CREATE"   DEFD   20 S" ALLOT"    DEFD
   \ These three cannot be written with S" -- one of them IS S" -- so
   \ their names are assembled a character at a time.
   21 83 34 NM2 DEFD                    \ S"
   22 92 NM1 DEFD                       \ backslash
   23 40 NM1 DEFD ;                     \ (

: BUILD-RUNTIME
   RT-STACK RT-MATH RT-COMPARE RT-REST RT-DIRECTIVES ;

\ -- reading the source --------------------------------------------------

CREATE PATHB 1024 ALLOT
: CSTR ( a n -- a' )                    \ open() wants a nul
   NQ !  PATHB NQ @ BMOVE  0 PATHB NQ @ + C!  PATHB ;

: SLURP-SOURCE
   SRCP @ OPENF DUP 0< IF S" cannot open the source" BAD THEN
   DUP SBUF @ SMAX READF  SLEN !
   CLOSEF
   SLEN @ 0 <= IF S" the source is empty" BAD THEN
   0 SPOS ! ;

: SCH ( -- c )   SBUF @ SPOS @ + C@ ;
: SEND? ( -- f ) SPOS @ SLEN @ >= ;
: SKIPWS  BEGIN SEND? IF 0 ELSE SCH 33 < THEN WHILE 1 SPOS +! REPEAT ;
: TOKEN ( -- a n )
   SKIPWS
   SBUF @ SPOS @ +
   BEGIN SEND? IF 0 ELSE SCH 32 > THEN WHILE 1 SPOS +! REPEAT
   SBUF @ SPOS @ + OVER - ;

VARIABLE NUMV  VARIABLE NUMS  VARIABLE NA  VARIABLE NN2
: ?NUM ( a n -- v f )
   NN2 ! NA !
   NN2 @ 0= IF 0 0 EXIT THEN
   0 NUMV ! 0 NUMS !
   NA @ C@ 45 = IF 1 NUMS ! 1 NA +! -1 NN2 +! THEN
   NN2 @ 0= IF 0 0 EXIT THEN
   NN2 @ 0 ?DO
      NA @ I + C@
      DUP 48 < OVER 57 > OR IF DROP 0 0 UNLOOP EXIT THEN
      48 - NUMV @ 10 * + NUMV !
   LOOP
   NUMV @ NUMS @ IF NEGATE THEN -1 ;

\ -- the two compile-time stacks -----------------------------------------
\ CTS holds values while the source is being INTERPRETED, which here means
\ only one thing: working out the size in  1024 CELLS CONSTANT CMAX. CSTK
\ holds open control structures while it is being COMPILED.

: CTPUSH ( n -- )  CTSP @ 32 < 0= IF S" too much arithmetic" BAD THEN
                   CTSP @ CELLS CTS + !  1 CTSP +! ;
: CTPOP ( -- n )   CTSP @ 0= IF S" a value was expected" BAD THEN
                   -1 CTSP +!  CTSP @ CELLS CTS + @ ;
: CPUSH ( m a -- ) CSP @ 126 < 0= IF S" control stack full" BAD THEN
                   CSP @ 1+ CELLS CSTK + !  CSP @ CELLS CSTK + !  2 CSP +! ;
: CPOP ( -- m a )  CSP @ 2 < IF S" unstructured" BAD THEN
                   -2 CSP +!
                   CSP @ CELLS CSTK + @  CSP @ 1+ CELLS CSTK + @ ;

: LIT,  ( n -- )    oIMM OP2, oPSH OP, hPUSH @ JSRC, 1 oADJ OP2, ;
: DLIT, ( off -- )  IMMD,     oPSH OP, hPUSH @ JSRC, 1 oADJ OP2, ;

\ -- the directives ------------------------------------------------------

: D-COLON
   TOKEN DUP 0= IF S" a name was expected" BAD THEN
   NSTORE  0 CN @ 0 DEFE
   CN @ CURW !
   1 CSTATE !  0 LOOPD !  0 CSP !
   0 oENT OP2, ;
: D-SEMI
   CSTATE @ 0= IF S" ; outside a definition" BAD THEN
   CSP @ 0<> IF S" an unfinished control structure" BAD THEN
   oLEV OP,  0 CSTATE ! ;
: D-IF     hPOPA @ JSRC, oBZ FMARK, 1 SWAP CPUSH ;
: D-ELSE   CPOP SWAP 1 <> IF S" ELSE without IF" BAD THEN
           oJMP FMARK, SWAP FRES  1 SWAP CPUSH ;
: D-THEN   CPOP SWAP 1 <> IF S" THEN without IF" BAD THEN  FRES ;
: D-BEGIN  3 CN @ CPUSH ;
: D-UNTIL  CPOP SWAP 3 <> IF S" UNTIL without BEGIN" BAD THEN
           hPOPA @ JSRC, oBZ SWAP BJ, ;
: D-AGAIN  CPOP SWAP 3 <> IF S" AGAIN without BEGIN" BAD THEN
           oJMP SWAP BJ, ;
: D-WHILE  hPOPA @ JSRC, oBZ FMARK, 4 SWAP CPUSH ;
: D-REPEAT CPOP SWAP 4 <> IF S" REPEAT without WHILE" BAD THEN
           CPOP SWAP 3 <> IF S" REPEAT without BEGIN" BAD THEN
           oJMP SWAP BJ,  FRES ;
: D-DO     hDO @ JSRC,  5 CN @ CPUSH  1 LOOPD +! ;
: D-QDO    hQDO @ JSRC, oBZ FMARK,  6 SWAP CPUSH  5 CN @ CPUSH  1 LOOPD +! ;
: D-CLOSE ( helper -- )                 \ the tail shared by LOOP and +LOOP
   JSRC,
   CPOP SWAP 5 <> IF S" LOOP without DO" BAD THEN
   oBNZ SWAP BJ,
   CSP @ 2 >= IF
      CSP @ 2 - CELLS CSTK + @ 6 = IF CPOP SWAP DROP FRES THEN
   THEN
   hUNLP @ JSRC,
   -1 LOOPD +! ;
: D-EXIT   oLEV OP, ;

: D-VARIABLE
   TOKEN DUP 0= IF S" a name was expected" BAD THEN
   NSTORE 3 1 CELLS DALLOT 0 DEFE ;
: D-CONSTANT
   TOKEN DUP 0= IF S" a name was expected" BAD THEN
   NSTORE 2 CTPOP 0 DEFE ;
: D-CREATE
   TOKEN DUP 0= IF S" a name was expected" BAD THEN
   NSTORE 3 0 DALLOT 0 DEFE ;
: D-ALLOT  CTPOP DALLOT DROP ;

VARIABLE STA  VARIABLE STN  VARIABLE STO
: D-STRING
   CSTATE @ 0= IF S" a string outside a definition" BAD THEN
   SEND? 0= IF SCH 32 = IF 1 SPOS +! THEN THEN
   SBUF @ SPOS @ +
   BEGIN SEND? IF 0 ELSE SCH 34 <> THEN WHILE 1 SPOS +! REPEAT
   SBUF @ SPOS @ + OVER -
   SEND? 0= IF 1 SPOS +! THEN
   STN ! STA !
   STN @ DALLOT STO !
   STA @ DBUF @ STO @ + STN @ BMOVE
   STO @ DLIT,  STN @ LIT, ;
: D-LINE   BEGIN SEND? IF 0 ELSE SCH 10 <> THEN WHILE 1 SPOS +! REPEAT ;
: D-PAREN  BEGIN SEND? IF 0 ELSE SCH 41 <> THEN WHILE 1 SPOS +! REPEAT
           SEND? 0= IF 1 SPOS +! THEN ;

: DODIR ( id -- )
   DUP  1 = IF DROP D-COLON    EXIT THEN
   DUP  2 = IF DROP D-SEMI     EXIT THEN
   DUP  3 = IF DROP D-IF       EXIT THEN
   DUP  4 = IF DROP D-ELSE     EXIT THEN
   DUP  5 = IF DROP D-THEN     EXIT THEN
   DUP  6 = IF DROP D-BEGIN    EXIT THEN
   DUP  7 = IF DROP D-UNTIL    EXIT THEN
   DUP  8 = IF DROP D-WHILE    EXIT THEN
   DUP  9 = IF DROP D-REPEAT   EXIT THEN
   DUP 10 = IF DROP D-AGAIN    EXIT THEN
   DUP 11 = IF DROP D-DO       EXIT THEN
   DUP 12 = IF DROP D-QDO      EXIT THEN
   DUP 13 = IF DROP hLOOP  @ D-CLOSE EXIT THEN
   DUP 14 = IF DROP hPLOOP @ D-CLOSE EXIT THEN
   DUP 15 = IF DROP D-EXIT     EXIT THEN
   DUP 17 = IF DROP D-VARIABLE EXIT THEN
   DUP 18 = IF DROP D-CONSTANT EXIT THEN
   DUP 19 = IF DROP D-CREATE   EXIT THEN
   DUP 20 = IF DROP D-ALLOT    EXIT THEN
   DUP 21 = IF DROP D-STRING   EXIT THEN
   DUP 22 = IF DROP D-LINE     EXIT THEN
   DUP 23 = IF DROP D-PAREN    EXIT THEN
   DROP S" an unknown directive" BAD ;

\ Constant folding, so that  1024 CELLS CONSTANT CMAX  needs no
\ interpreter -- there is none here, and a compiler that runs the
\ program it is compiling is a different kind of thing.
: FOLD ( f -- )
   DUP  1 = IF DROP CTPOP CTPOP *          CTPUSH EXIT THEN
   DUP  2 = IF DROP CTPOP CTPOP +          CTPUSH EXIT THEN
   DUP  3 = IF DROP CTPOP NEGATE CTPOP +   CTPUSH EXIT THEN
   DUP  4 = IF DROP CTPOP 1 CELLS *        CTPUSH EXIT THEN
   DUP  5 = IF DROP CTPOP 1 CELLS +        CTPUSH EXIT THEN
   DUP  6 = IF DROP                               EXIT THEN
   DUP  7 = IF DROP CTPOP 1+               CTPUSH EXIT THEN
   DUP  8 = IF DROP CTPOP 1+               CTPUSH EXIT THEN
   DUP  9 = IF DROP CTPOP 1-               CTPUSH EXIT THEN
   DUP 10 = IF DROP CTPOP 2*               CTPUSH EXIT THEN
   DROP S" that cannot be folded" BAD ;

VARIABLE WI
: DOWORD ( i -- )
   WI !
   WI @ W[] 2 CELLS + @
   DUP 4 = IF DROP WI @ W[] 3 CELLS + @ DODIR EXIT THEN
   CSTATE @ 0= IF
      DUP 2 = IF DROP WI @ W[] 3 CELLS + @ CTPUSH EXIT THEN
      DROP
      WI @ W[] 4 CELLS + @ DUP IF FOLD EXIT THEN DROP
      S" that word only means something inside a definition" BAD EXIT
   THEN
   DUP 0 = IF DROP WI @ W[] 3 CELLS + @ JSRC, EXIT THEN
   DUP 2 = IF DROP WI @ W[] 3 CELLS + @ LIT,  EXIT THEN
   DUP 3 = IF DROP WI @ W[] 3 CELLS + @ DLIT, EXIT THEN
   DROP S" a word of no kind at all" BAD ;

VARIABLE TA  VARIABLE TN
: STEP ( -- f )
   TOKEN TN ! TA !
   TN @ 0= IF 0 EXIT THEN
   TA @ TN @ FINDW DUP 0< IF
      DROP
      TA @ TN @ ?NUM IF
         CSTATE @ IF LIT, ELSE CTPUSH THEN
      ELSE
         DROP TA @ TN @ BAD
      THEN
   ELSE
      DOWORD
   THEN
   -1 ;
: COMPILE-SOURCE  BEGIN STEP 0= UNTIL ;

\ -- the entry stub ------------------------------------------------------
\ load-c4r.c calls the entry as entry(argc, argv), an ordinary C4
\ function, so the stub is where the three software stacks get their
\ pointers and where argv is handed to the program -- if the program
\ declared somewhere to put it. Emitted last, so MAIN is already known.

: FINISH
   S" MAIN" FINDW DUP 0< IF S" the program has no MAIN" BAD THEN
   W[] 3 CELLS + @
   CN @ ENTRYW !
   0 oENT OP2,
   S" ARGC" FINDW DUP 0< IF DROP ELSE
      W[] 3 CELLS + @ IMMD, oPSH OP, 3 oLEA OP2, oLI OP, oSI OP, THEN
   S" ARGV" FINDW DUP 0< IF DROP ELSE
      W[] 3 CELLS + @ IMMD, oPSH OP, 2 oLEA OP2, oLI OP, oSI OP, THEN
   DSPO @ IMMD, oPSH OP, DSBO @ IMMD, oSI OP,
   RSPO @ IMMD, oPSH OP, RSBO @ IMMD, oSI OP,
   LSPO @ IMMD, oPSH OP, LSBO @ IMMD, oSI OP,
   JSRC,
   0 oIMM OP2,
   oLEV OP, ;

\ -- writing it out ------------------------------------------------------
\ Byte by byte, in the host's own order, because the header is thirteen
\ bytes long and everything after it is therefore unaligned in the file.
\ To standard output, not to a file: the C4 VM has no write syscall, so a
\ compiled compiler could not open one -- and a compiler that writes to
\ stdout is the same compiler on both sides of the bootstrap.

CREATE WSCR 1 CELLS ALLOT
: FW ( w -- )  WSCR !  1 CELLS 0 ?DO WSCR I + C@ EMIT LOOP ;
: FMK ( c -- ) EMIT  1 CELLS 1- 0 ?DO 0 EMIT LOOP ;

: WRITE-IMAGE
   67 EMIT 52 EMIT 82 EMIT               \ "C4R"
   2 EMIT                                \ version 2
   1 CELLS 8 * EMIT                      \ word bits
   8 0 ?DO 112 EMIT LOOP                 \ eight bytes of 'p'
   ENTRYW @ FW
   CN @ FW
   DN @ FW
   PN @ FW
   0 FW  0 FW  0 FW                      \ symbols, constructors, destructors
   67 FMK  CN @ 0 ?DO I CELLS CBUF @ + @ FW LOOP
   68 FMK  DN @ 0 ?DO I DBUF @ + C@ EMIT LOOP
   80 FMK  PN @ 3 * 0 ?DO I CELLS PBUF @ + @ FW LOOP
   99 FMK  100 FMK  83 FMK ;

\ -- putting it together -------------------------------------------------

: ALLOC-ALL
   CMAX CELLS ALLOCATE CBUF !
   DMAX ALLOCATE DBUF !
   PMAX 3 * CELLS ALLOCATE PBUF !
   SMAX ALLOCATE SBUF !
   NMAX ALLOCATE NBLOB !
   WMAX WSZ * CELLS ALLOCATE DICT !
   DBUF @ DMAX 0 BFILL
   0 CBUF @ !
   1 CN ! 0 DN ! 0 PN ! 0 NBN ! 0 DICTN !
   0 CSTATE ! 0 CSP ! 0 CTSP ! 0 SPOS ! ;

: PICK-SOURCE
   SRCP @ IF EXIT THEN
   ARGC @ 1 > IF ARGV @ 1 CELLS + @ SRCP ! EXIT THEN
   S" src/c4th/forth/self.f" CSTR SRCP ! ;

: MAIN
   ALLOC-ALL
   RTDATA
   BUILD-RUNTIME
   PICK-SOURCE
   SLURP-SOURCE
   COMPILE-SOURCE
   FINISH
   WRITE-IMAGE ;
