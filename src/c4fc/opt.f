\ c4fc opt.f -- the peephole optimizer, matching c4opt.lisp.
\
\ The passes cannot work on a finished image: deleting one instruction
\ moves every address after it. So this decodes the image into the
\ LABELLED form c4opt works on -- branch targets as label ids rather
\ than addresses -- optimises that, and assembles it again.
\
\ Decoding is c4r.lisp's reconstruction, and it has to be exactly that,
\ because a different set of labels is a different program shape. A code
\ offset becomes a label if anything points at it: the entry, the value
\ of a -1 patch, the value of a -3 patch (a switch table entry names
\ code from data), a constructor or destructor entry, or a defined
\ function's symbol.
\
\ Round-tripping with no passes at all must reproduce the image byte for
\ byte, and that is checked before any pass is trusted.

262144 CONSTANT MAXI
VARIABLE IBUF  VARIABLE IN#
VARIABLE JBUF  VARIABLE JN#
VARIABLE LMAP                           \ label id -> address, when encoding
VARIABLE ISLBL                          \ code offset -> is it a label
VARIABLE MAXLBL

\ item: kind, op, arg, argkind
0 CONSTANT k_insn   1 CONSTANT k_label
0 CONSTANT a_none   1 CONSTANT a_plain  2 CONSTANT a_code  3 CONSTANT a_data
\ An unresolved reference: the argument is the placeholder patch type
\ itself, carried through untouched -- the symbol it names has no
\ address in this unit, so nothing here can move it.
4 CONSTANT a_ext

VARIABLE PMAP                           \ code address -> its patch, or 0
: I[] ( buf i -- a )  4 CELLS * + ;
: I.K ( a -- a )  ;
: I.O ( a -- a )  1 CELLS + ;
: I.A ( a -- a )  2 CELLS + ;
: I.T ( a -- a )  3 CELLS + ;

: OPT-INIT
   MAXI 4 CELLS * ALLOCATE IBUF !
   MAXI 4 CELLS * ALLOCATE JBUF !
   MAXI CELLS ALLOCATE LMAP !
   CMAX ALLOCATE ISLBL !
   CMAX CELLS ALLOCATE PMAP ! ;

: ITEM, ( buf n kind op arg at -- n' ) {: b n k o a t | p -- n :}
   n MAXI < 0= IF ." c4fc: too many instructions to optimise" CR ABORT THEN
   b n I[] TO p
   k p I.K !   o p I.O !   a p I.A !   t p I.T !
   n 1+ ;

\ -- decoding ------------------------------------------------------------

: MARK-LABEL ( off -- )  ISLBL @ + 1 SWAP C! ;
: LABEL? ( off -- f )    ISLBL @ + C@ 0<> ;

: MARK-LABELS {: | y p -- :}
   ISLBL @ CMAX 0 FILL
   ENTRY @ 0< 0= IF ENTRY @ MARK-LABEL THEN
   PN @ 0 ?DO
      I 3 * CELLS PATCH @ + TO p
      p @ -1 = p @ -3 = OR IF p 2 CELLS + @ MARK-LABEL THEN
   LOOP
   CONSN @ 0 ?DO I CELLS CONS @ + @ MARK-LABEL LOOP
   DESN  @ 0 ?DO I CELLS DESS @ + @ MARK-LABEL LOOP
   SN @ 0 ?DO
      I SYM[] TO y
      y y.class @ 129 = IF y y.val @ MARK-LABEL THEN
   LOOP ;

\ The patch aimed at this word, or 0.
\
\ This was a scan of the whole patch list per instruction, which is a
\ product: twenty-five thousand instructions against two and a half
\ thousand patches is sixty million comparisons, and it cost more than
\ every other phase of -O put together. One walk of the patch list fills
\ a map indexed by code address instead, and the question becomes a
\ fetch. Only CODE-RESIDENT patches go in it; the data-resident ones
\ address the data segment and would collide.
: PMAP-BUILD ( -- ) {: | p -- :}
   PMAP @ CMAX CELLS 0 FILL
   PN @ 0 ?DO
      I 3 * CELLS PATCH @ + TO p
      p @ -1 = p @ -2 = OR p @ -1000 <= OR IF
         p  p CELL+ @ CELLS PMAP @ + !
      THEN
   LOOP ;
: PATCH-AT ( off -- p|0 )  CELLS PMAP @ + @ ;

\ LEA..ADJ carry an operand, and so do JSRI and JSRS -- which c4fc did
\ not emit until it had to compile a call through a function pointer.
\ Getting this list wrong does not corrupt the code, because the operand
\ word round-trips as if it were an instruction; it silently drops the
\ PATCH on it, and the call then goes to wherever address zero is.
: HAS-OPERAND? ( op -- f ) {: o -- f :}
   o 0< IF 0 EXIT THEN
   o 7 <= IF -1 EXIT THEN
   o oJSRI = o oJSRS = OR ;

: DECODE {: | i n w p op -- :}
   MARK-LABELS
   PMAP-BUILD
   0 TO n   0 TO i
   BEGIN i CN @ < WHILE
      i LABEL? IF IBUF @ n k_label i 0 a_none ITEM, TO n THEN
      i CELLS CODE @ + @ TO w
      w HAS-OPERAND?  i 1+ CN @ < AND  i 1+ LABEL? 0= AND IF
         i 1+ PATCH-AT TO p
         p IF
            p @ -1 = IF IBUF @ n k_insn w  p 2 CELLS + @ a_code ITEM, TO n ELSE
            p @ -1000 <= IF IBUF @ n k_insn w  p @ a_ext ITEM, TO n
                     ELSE IBUF @ n k_insn w  p 2 CELLS + @ a_data ITEM, TO n THEN THEN
         ELSE
            IBUF @ n k_insn w  i 1+ CELLS CODE @ + @  a_plain ITEM, TO n
         THEN
         i 2 + TO i
      ELSE
         IBUF @ n k_insn w 0 a_none ITEM, TO n
         i 1+ TO i
      THEN
   REPEAT
   n IN# !
   CN @ MAXLBL ! ;

\ -- encoding ------------------------------------------------------------

: ASSIGN-ADDRESSES {: | i n a p -- :}
   LMAP @ MAXI CELLS 0 FILL
   0 TO a
   IN# @ 0 ?DO
      IBUF @ I I[] TO p
      p I.K @ k_label = IF a  p I.O @ CELLS LMAP @ + !
                        ELSE a p I.T @ a_none = IF 1 ELSE 2 THEN + TO a THEN
   LOOP ;
: LADDR ( labelid -- addr )  CELLS LMAP @ + @ ;

: ENCODE {: | p y -- :}
   ASSIGN-ADDRESSES
   0 CN !   0 PN !
   IN# @ 0 ?DO
      IBUF @ I I[] TO p
      p I.K @ k_insn = IF
         p I.T @ a_none = IF p I.O @ C, ELSE
         p I.T @ a_plain = IF p I.A @ p I.O @ OP2, ELSE
         p I.T @ a_code = IF
            p I.O @ OP,  -1 CHERE p I.A @ LADDR PAT,  p I.A @ LADDR C,
         ELSE
         p I.T @ a_ext = IF
            p I.O @ OP,  p I.A @ CHERE 0 PAT,  0 C,
         ELSE
            p I.O @ OP,  -2 CHERE p I.A @ PAT,  0 C,
         THEN THEN THEN THEN
      THEN
   LOOP
   ENTRY @ 0< 0= IF ENTRY @ LADDR ENTRY ! THEN
   CONSN @ 0 ?DO I CELLS CONS @ + DUP @ LADDR SWAP ! LOOP
   DESN  @ 0 ?DO I CELLS DESS @ + DUP @ LADDR SWAP ! LOOP
   SN @ 0 ?DO
      I SYM[] TO y
      y y.class @ 129 = IF y y.val @ LADDR y y.val ! THEN
   LOOP
   \ the data-resident patches come last, as they did before -- jump
   \ tables, `int *fp = &fn;` and `char *s = "..."` alike, with the code
   \ addresses among them remapped through LADDR
   ['] LADDR IS TAB-MAP
   DB2 @ EMIT-TABPATS
   ['] TAB-SAME IS TAB-MAP ;

\ -- reading the item list ----------------------------------------------

: SWAP-BUFS  IBUF @ JBUF @ IBUF ! JBUF !  JN# @ IN# ! ;
: EMIT-ITEM ( kind op arg at -- )  {: k o a t -- :}
   JBUF @ JN# @ k o a t ITEM, JN# ! ;
: COPY-ITEM ( i -- ) {: i | p -- :}
   IBUF @ i I[] TO p
   p I.K @ p I.O @ p I.A @ p I.T @ EMIT-ITEM ;
: IK ( i -- k )   IBUF @ SWAP I[] I.K @ ;
: IOP ( i -- op ) IBUF @ SWAP I[] I.O @ ;
: IARG ( i -- a ) IBUF @ SWAP I[] I.A @ ;
: ITY ( i -- t )  IBUF @ SWAP I[] I.T @ ;
: LBL? ( i -- f )  IK k_label = ;
: BARE? ( i op -- f ) {: i o -- f :}          \ this opcode, no operand
   i LBL? IF 0 EXIT THEN
   i IOP o = i ITY a_none = AND ;
: PLAINOP? ( i op -- f ) {: i o -- f :}       \ this opcode, plain operand
   i LBL? IF 0 EXIT THEN
   i IOP o = i ITY a_plain = AND ;
: CODEOP? ( i op -- f ) {: i o -- f :}        \ this opcode, code target
   i LBL? IF 0 EXIT THEN
   i IOP o = i ITY a_code = AND ;
: ANYBARE? ( i -- f )                          \ any opcode with no operand
   DUP LBL? IF DROP 0 EXIT THEN  ITY a_none = ;

\ -- fold ---------------------------------------------------------------
\ The window is IMM a; PSH; IMM b; OP, and the stack operand is on the
\ LEFT, so the value is a OP b. A folded IMM is fed back in: constants
\ cascade. FOLD1 itself lives in emit.f, because the TREE pass folds
\ the same operators on the same rules before any of this runs.

: PASS-FOLD {: | i j pv go ok v -- :}
   0 JN# !   0 TO i
   BEGIN i IN# @ < WHILE
      i oIMM PLAINOP? 0= IF
         i COPY-ITEM   i 1+ TO i
      ELSE
         i IARG TO pv   i 1+ TO j   1 TO go
         BEGIN
            go
            j 2 + IN# @ < AND
            j oPSH BARE? AND
            j 1+ oIMM PLAINOP? AND
            j 2 + ANYBARE? AND
         WHILE
            pv  j 1+ IARG  j 2 + IOP  FOLD1 TO ok TO v
            ok IF v TO pv  j 3 + TO j  ELSE 0 TO go THEN
         REPEAT
         k_insn oIMM pv a_plain EMIT-ITEM
         j TO i
      THEN
   REPEAT
   SWAP-BUFS ;

\ -- shl ----------------------------------------------------------------
\ PSH; IMM 8; MUL -> PSH; IMM 3; SHL, which every pointer subscript emits.

: PASS-SHL {: | i -- :}
   0 JN# !   0 TO i
   BEGIN i IN# @ < WHILE
      i oPSH BARE?
      i 2 + IN# @ < AND
      i 1+ oIMM PLAINOP? AND
      IF i 1+ IARG 1 CELLS = i 2 + oMUL BARE? AND ELSE 0 THEN
      IF
         k_insn oPSH 0 a_none EMIT-ITEM
         k_insn oIMM 3 a_plain EMIT-ITEM
         k_insn oSHL 0 a_none EMIT-ITEM
         i 3 + TO i
      ELSE
         i COPY-ITEM  i 1+ TO i
      THEN
   REPEAT
   SWAP-BUFS ;

\ -- adj0 ---------------------------------------------------------------

: PASS-ADJ0 {: | i -- :}
   0 JN# !   0 TO i
   BEGIN i IN# @ < WHILE
      i oADJ PLAINOP? IF i IARG 0= ELSE 0 THEN
      0= IF i COPY-ITEM THEN
      i 1+ TO i
   REPEAT
   SWAP-BUFS ;

\ -- jmpnext ------------------------------------------------------------
\ A JMP to a label that the very next items reach by falling through.

: LABEL-FOLLOWS? ( t i -- f ) {: t i -- f :}
   BEGIN i IN# @ < WHILE
      i LBL? 0= IF 0 EXIT THEN
      i IOP t = IF -1 EXIT THEN
      i 1+ TO i
   REPEAT 0 ;
: PASS-JMPNEXT {: | i -- :}
   0 JN# !   0 TO i
   BEGIN i IN# @ < WHILE
      i oJMP CODEOP? IF i IARG i 1+ LABEL-FOLLOWS? ELSE 0 THEN
      0= IF i COPY-ITEM THEN
      i 1+ TO i
   REPEAT
   SWAP-BUFS ;

\ -- thread -------------------------------------------------------------
\ A jump to a label whose only content is JMP L2 can go to L2 directly.
\ One level, and never a self-loop.

VARIABLE FT
: MAX-LABEL ( -- n ) {: | m -- m :}
   0 TO m
   IN# @ 0 ?DO I LBL? IF I IOP m > IF I IOP TO m THEN THEN LOOP
   m ;
: FOLLOWING-JMP ( i -- t|-1 ) {: i -- t :}
   BEGIN i IN# @ < WHILE
      i LBL? 0= IF
         i oJMP CODEOP? IF i IARG EXIT THEN
         -1 EXIT
      THEN
      i 1+ TO i
   REPEAT -1 ;
: PASS-THREAD {: | i t f -- :}
   FT @ 0= IF MAXI CELLS ALLOCATE FT ! THEN
   FT @ MAXI CELLS 0 FILL
   IN# @ 0 ?DO
      I LBL? IF
         I 1+ FOLLOWING-JMP TO t
         t 0< 0= IF t 1+ I IOP CELLS FT @ + ! THEN
      THEN
   LOOP
   0 JN# !   0 TO i
   BEGIN i IN# @ < WHILE
      i oJMP CODEOP? i oBZ CODEOP? OR i oBNZ CODEOP? OR IF
         i IARG TO t
         t CELLS FT @ + @ TO f
         f 0> IF f 1- t <> IF
            k_insn i IOP f 1- a_code EMIT-ITEM
            i 1+ TO i
            0
         ELSE 1 THEN ELSE 1 THEN
         IF i COPY-ITEM i 1+ TO i THEN
      ELSE
         i COPY-ITEM  i 1+ TO i
      THEN
   REPEAT
   SWAP-BUFS ;

\ -- dead ---------------------------------------------------------------
\ After JMP, LEV or JMPA nothing runs until a label.

: PASS-DEAD {: | i drop? -- :}
   0 JN# !   0 TO i   0 TO drop?
   BEGIN i IN# @ < WHILE
      i LBL? IF 0 TO drop? THEN
      drop? 0= IF
         i COPY-ITEM
         i oJMP CODEOP? i oLEV BARE? OR i oJMPA BARE? OR IF 1 TO drop? THEN
      THEN
      i 1+ TO i
   REPEAT
   SWAP-BUFS ;

\ -- tail calls ----------------------------------------------------------
\ JSR f; LEV  ->  ADJ (m - k); JMP f+2, where m is the enclosing ENT's
\ operand and k the callee's: the ADJ moves sp from bp-m to bp-k, bp is
\ untouched, so the callee's locals sit in our frame and its LEV pops OUR
\ saved pair and returns to our caller. Tail recursion becomes O(1)
\ stack. Variadic callees are excluded -- their argument slots would be
\ read out of the caller's frame.

VARIABLE EMM  VARIABLE SMM  VARIABLE BANM
: EM[] ( l -- a )  CELLS EMM @ + ;
: SM[] ( l -- a )  CELLS SMM @ + ;
: BAN[] ( l -- a ) BANM @ + ;

: TAIL-SITE ( i -- f|-1 ) {: i | f -- f :}
   i oJSR CODEOP? 0= IF -1 EXIT THEN
   i 1+ IN# @ >= IF -1 EXIT THEN
   i 1+ oLEV BARE? 0= IF -1 EXIT THEN
   i IARG TO f
   f EM[] @ 0= IF -1 EXIT THEN
   f BAN[] C@ 1 = IF -1 EXIT THEN
   f ;
: TAIL-GO ( i -- f|-1 ) {: i | f -- f :}
   i TAIL-SITE TO f
   f 0< IF -1 EXIT THEN
   f SM[] @ 0> IF f ELSE -1 THEN ;

: PASS-TAIL {: | i f maxl nextid curm pending y -- :}
   EMM @ 0= IF MAXI CELLS ALLOCATE EMM !  MAXI CELLS ALLOCATE SMM !
               MAXI ALLOCATE BANM ! THEN
   EMM @ MAXI CELLS 0 FILL   SMM @ MAXI CELLS 0 FILL   BANM @ MAXI 0 FILL
   MAX-LABEL TO maxl
   SN @ 0 ?DO
      I SYM[] TO y
      y y.class @ 129 = IF
         y y.val @ maxl <= IF
            y y.agg @ 32 AND 0<> IF 1 y y.val @ BAN[] C! THEN
         THEN
      THEN
   LOOP
   IN# @ 0 ?DO
      I LBL? IF
         I 1+ IN# @ < IF
            I 1+ oENT PLAINOP? IF I 1+ IARG 1+ I IOP EM[] ! THEN
         THEN
      THEN
   LOOP
   maxl 1+ TO nextid
   IN# @ 0 ?DO
      I TAIL-SITE TO f
      f 0< 0= IF
         f SM[] @ 0= IF  nextid 1+ f SM[] !  nextid 1+ TO nextid  THEN
      THEN
   LOOP
   0 TO curm   0 TO pending
   0 JN# !   0 TO i
   BEGIN i IN# @ < WHILE
      i TAIL-GO TO f
      f 0< 0= curm 0> AND IF
         k_insn oADJ  curm 1- f EM[] @ 1- -  a_plain EMIT-ITEM
         k_insn oJMP  f SM[] @ 1-  a_code EMIT-ITEM
         i 2 + TO i
      ELSE
         i LBL? IF i IOP SM[] @ ?DUP IF TO pending THEN THEN
         i oENT PLAINOP? IF i IARG 1+ TO curm THEN
         i COPY-ITEM
         i oENT PLAINOP? pending 0> AND IF
            k_label pending 1- 0 a_none EMIT-ITEM
            0 TO pending
         THEN
         i 1+ TO i
      THEN
   REPEAT
   SWAP-BUFS ;

\ -- the fixpoint --------------------------------------------------------
\ Threading enables jmpnext which enables dead, and folds cascade, so the
\ passes run until the instruction count stops falling -- bounded at ten
\ rounds. adj0 runs again after tail because a same-size tail transform
\ emits ADJ 0.

: COUNT-INSNS ( -- n ) {: | n -- n :}
   0 TO n
   IN# @ 0 ?DO I LBL? 0= IF n 1+ TO n THEN LOOP
   n ;
\ -- fuse ----------------------------------------------------------------
\ docs/fused-opcodes.md. Two- and three-instruction windows collapsed
\ into one opcode, LONGEST FIRST -- LEA;LI;PSH is PSHL, not LDL then a
\ stray PSH. The fused instruction keeps the operand AND the operand
\ kind of the one it starts with, so an IMM of a data address fuses to a
\ PSHG of the same data address and the patch survives.
\
\ It runs ONCE, after the fixpoint, so that no other pass has to
\ understand the fused forms and so that fusing never hides a fold from
\ the round that would have followed. A label ends a window, which is
\ the same reason every other pass here is safe.

: FUSE2 ( a b -- op ) {: a b -- op :}    \ two-instruction windows, or -1
   b oLI = IF  a oLEA = IF oLDL EXIT THEN  a oIMM = IF oLDG EXIT THEN
               a oADD = IF oADDL EXIT THEN  THEN
   b oPSH = IF  a oLEA = IF oLEAP EXIT THEN  a oIMM = IF oIMMP EXIT THEN
                a oLI  = IF oLIP EXIT THEN  THEN
   -1 ;

: PASS-FUSE {: | i a b c op -- :}
   0 JN# !   0 TO i
   BEGIN i IN# @ < WHILE
      i LBL? IF i COPY-ITEM  i 1+ TO i
      ELSE
         i IOP TO a
         i 1+ IN# @ < IF i 1+ LBL? IF -1 ELSE i 1+ IOP THEN ELSE -1 THEN TO b
         i 2 + IN# @ < IF i 2 + LBL? IF -1 ELSE i 2 + IOP THEN ELSE -1 THEN TO c
         c oPSH = b oLI = AND  a oLEA = a oIMM = OR AND IF
            k_insn  a oLEA = IF oPSHL ELSE oPSHG THEN
            i IARG  i ITY  EMIT-ITEM
            i 3 + TO i
         ELSE
            a b FUSE2 TO op
            op 0< IF i COPY-ITEM  i 1+ TO i
            ELSE
               op oLIP = op oADDL = OR IF
                  k_insn op 0 a_none EMIT-ITEM
               ELSE
                  k_insn op  i IARG  i ITY  EMIT-ITEM
               THEN
               i 2 + TO i
            THEN
         THEN
      THEN
   REPEAT
   SWAP-BUFS ;

: OPT-PASSES
   PASS-FOLD PASS-SHL PASS-ADJ0 PASS-JMPNEXT PASS-THREAD PASS-TAIL
   PASS-ADJ0 PASS-DEAD ;

: OPT-RUN {: | n prev rounds -- :}
   OPT-INIT DECODE
   COUNT-INSNS TO n   -1 TO prev   0 TO rounds
   BEGIN n prev <> rounds 10 < AND WHILE
      OPT-PASSES
      n TO prev   COUNT-INSNS TO n   rounds 1+ TO rounds
   REPEAT
   FUSE @ IF PASS-FUSE THEN
   ENCODE ;
