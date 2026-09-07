\ c4fc inline.f -- the inliner (docs/inline-small-functions.md, I7).
\
\ A call in c4 costs JSR, ENT, ADJ and LEV whatever is on the other side
\ of it, plus a push per argument and a bp-relative load for every
\ parameter the body reads. For a one-line accessor that is most of the
\ cost of using one.
\
\ THIS PORT IS SIMPLER THAN c4lc's, and the reason is worth stating.
\
\ c4lc rewrites the AST: it substitutes arguments, renames locals, and
\ turns `return E` into an assignment plus a jump. c4fc does none of
\ that, because it EMITS as it walks and GEN is read-only over the tree
\ (there is not one store into a node in gen.f or emit.f). So a body is
\ spliced by pointing the callee's symbols at fresh slots in the
\ CALLER's frame and generating the SAME tree again. No copy, no
\ rewrite, no substitution.
\
\ Two things fall out for free:
\
\   * `return` needs no rewriting. c4's result lives in A, and a spliced
\     `return E` leaves E in A exactly as the call would have -- so it
\     only has to jump instead of LEV. That is one flag and one branch
\     in STMT n_ret, where c4lc needed a whole second pass (T4b) with
\     two AST node kinds of its own.
\   * and because the value arrives the same way, a splice works in
\     EXPRESSION position. c4lc's T4 is restricted to `f(a);` and
\     `x = f(a);`; this one is not.
\
\ What it shares is the rule that cost c4lc a miscompilation: a function
\ whose ADDRESS IS TAKEN is closed to the inliner in BOTH directions --
\ never spliced, and never spliced into. C4KE enters a custom opcode
\ handler by reading its ENT operand and jumping past it
\ (`__c4_adjust(*(handler - 1) * -1); __c4_jmp(handler);`), so the frame
\ that ENT describes is a contract with code somewhere else, and growing
\ it rewrites one side of that contract only.

VARIABLE INLINING   0 INLINING !         \ -minline
VARIABLE NINLINE    0 NINLINE !          \ splices performed

\ How big a body is worth splicing, measured in TOKENS -- the span the
\ parser consumed for it. c4lc counts AST nodes and needs a walk to do
\ it; here the token cursor is already a size, for nothing.
\
\ 40 because a node is roughly one to two tokens, so it is the same
\ neighbourhood as the 60 AST nodes c4lc measured on C4KE. That is a
\ MATCHED default, not an independently measured one: c4fc has no
\ speed benchmark of its own (docs/c4fc-design.md is explicit that it
\ is not a speed play), so the honest thing is to inherit the figure
\ that was measured and say so. What IS measured here is the cost of
\ getting it wrong -- on the stress corpus the image goes 9,502 bytes
\ at 24, 13,469 at 32, 16,029 at 60, so this grows code quickly and
\ the budget is the whole of the control over it.
VARIABLE INLMAX     40 INLMAX !

\ ---- functions whose address is taken --------------------------------
\ Kept by NAME rather than by symbol, because UNIT-RESET reallocates the
\ symbol table between the two passes and a mark made in pass one has to
\ survive into pass two -- exactly as T2's root and reference tables do.
256 CONSTANT ATK-MAX
VARIABLE ATK#    0 ATK# !
VARIABLE ATK-A                            \ name pointer
VARIABLE ATK-U                            \ name length

: ATK-RESET ( -- )
   0 ATK# !
   ATK-MAX CELLS ALLOT: ATK-A !
   ATK-MAX CELLS ALLOT: ATK-U ! ;

: ATK? ( a u -- f ) {: a u -- f :}
   ATK# @ 0 ?DO
      I CELLS ATK-U @ + @ u = IF
         I CELLS ATK-A @ + @  a u BYTES= IF TRUE UNLOOP EXIT THEN
      THEN
   LOOP FALSE ;

: ATK, ( a u -- ) {: a u -- :}
   a u ATK? IF EXIT THEN
   ATK# @ ATK-MAX < IF
      a ATK# @ CELLS ATK-A @ + !
      u ATK# @ CELLS ATK-U @ + !
      1 ATK# +!
   THEN ;

\ ---- the candidate table ---------------------------------------------
\ Parallel arrays rather than a record, which is what the rest of this
\ compiler does with small fixed tables (PMAP and LMAP in opt.f).
64 CONSTANT IC-MAX
VARIABLE IC#     0 IC# !
VARIABLE IC-FN                            \ the function's symbol
VARIABLE IC-BODY                          \ its body node
VARIABLE IC-SB                            \ first symbol index: params, then locals
VARIABLE IC-SN                            \ how many of them
VARIABLE IC-NFIX                          \ how many of those are parameters

: INL-RESET ( -- )
   0 IC# !  0 NINLINE !
   IC-MAX CELLS ALLOT: IC-FN !
   IC-MAX CELLS ALLOT: IC-BODY !
   IC-MAX CELLS ALLOT: IC-SB !
   IC-MAX CELLS ALLOT: IC-SN !
   IC-MAX CELLS ALLOT: IC-NFIX !
   ATK-RESET ;

: IC[] ( v i -- a )  CELLS SWAP @ + ;

: IC-FIND ( y -- i|-1 ) {: y -- i :}
   IC# @ 0 ?DO  y IC-FN I IC[] @ = IF I UNLOOP EXIT THEN  LOOP  -1 ;

\ Record one. `sb` is the symbol index the parameters start at, `sn`
\ counts parameters AND locals -- they are contiguous, because PARAMS
\ declares its symbols and then the body's declarations follow, and
\ nothing between them declares anything else.
: IC, ( y body sb sn nfix -- ) {: y b sb sn nf -- :}
   IC# @ IC-MAX < 0= IF EXIT THEN
   y  IC-FN   IC# @ IC[] !
   b  IC-BODY IC# @ IC[] !
   sb IC-SB   IC# @ IC[] !
   sn IC-SN   IC# @ IC[] !
   nf IC-NFIX IC# @ IC[] !
   1 IC# +! ;

\ ---- returns inside a splice -----------------------------------------
\ Non-zero while a spliced body is being generated. It does two jobs and
\ they belong together: it tells n_ret to jump instead of LEV, and it
\ stops a splice happening inside a splice. One level only -- nothing
\ bounds a cascade but the call graph happening to be acyclic, and a
\ self-recursive candidate would splice into itself forever.
VARIABLE INLRET     0 INLRET !
64 CONSTANT INLM-MAX
VARIABLE INLM#      0 INLM# !
VARIABLE INLMARKS

: INLM-RESET ( -- )  INLM-MAX CELLS ALLOT: INLMARKS !  0 INLM# ! ;
: INLM, ( m -- )
   INLM# @ INLM-MAX < IF
      INLM# @ CELLS INLMARKS @ + !  1 INLM# +!
   ELSE DROP THEN ;

\ every `return` in the body jumped here; resolve them all to this point
: INLM-RES ( base -- ) {: b -- :}
   INLM# @ b ?DO  I CELLS INLMARKS @ + @ >RES  LOOP
   b INLM# ! ;

\ ---- the splice ------------------------------------------------------

\ Slots for the callee's parameters and locals, in the CALLER's frame.
\ A local's val is its slot NEGATED (parse.f), so a fresh one is just
\ the next NLOC.
\
\ Reserving and BINDING are deliberately two steps, with the arguments
\ evaluated in between. An argument can contain a call that is itself
\ spliced -- sq(sq(2)) -- and the inner splice retargets the very same
\ symbols, because it is the same function. Binding before evaluating
\ the arguments therefore has the inner splice overwrite the outer
\ one's slots, and the outer body then reads the inner's: sq(sq(2))
\ quietly returns 4 instead of 16. Assigning y.val last is what makes
\ nesting safe, and it costs nothing.
: INL-RESERVE ( i -- first ) {: i | n f -- f :}
   IC-SN i IC[] @ TO n
   NLOC @ 1+ TO f
   NLOC @ n + NLOC !
   f ;

\ Each argument is evaluated ONCE, in order, into its parameter's slot
\ -- addressed by NUMBER, since y.val is not bound yet. That is what
\ makes this safe where c4lc's substitution needed purity rules: an
\ argument here may call, assign and increment freely.
: INL-ARGS ( n i first -- ) {: n i f | k -- :}
   n >argn @ TO k
   k 0 ?DO
      f I + NEGATE oLEA OP2,
      oPSH OP,
      n >args @ I CELLS + @ GEN
      oSI OP,
   LOOP ;

: INL-BIND ( i first -- ) {: i f | b n -- :}
   IC-SB i IC[] @ TO b
   IC-SN i IC[] @ TO n
   n 0 ?DO  f I + NEGATE  b I + ST[] y.val !  LOOP ;

: INL-SPLICE ( n i -- ) {: n i | old base f -- :}
   i INL-RESERVE TO f
   n i f INL-ARGS
   i f INL-BIND
   INLRET @ TO old   1 INLRET !
   INLM# @ TO base
   IC-BODY i IC[] @ STMT
   base INLM-RES
   old INLRET !
   1 NINLINE +! ;

\ May this call be spliced? -1 for no, otherwise the candidate index.
\ The argument count must match exactly: c4 lets a call pass fewer than
\ the callee declares and the callee simply reads rubbish, which is a
\ thing a CALL can survive and a splice cannot.
: INL-TRY ( n -- i ) {: n | f i -- i :}
   INLINING @ 0= IF -1 EXIT THEN
   INLRET @ IF -1 EXIT THEN
   n >fn @ TO f
   f IC-FIND TO i
   i 0< IF -1 EXIT THEN
   n >argn @ IC-NFIX i IC[] @ = 0= IF -1 EXIT THEN
   i ;
