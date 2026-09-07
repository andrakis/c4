;; c4lc-tree.lisp -- AST-level optimizations for c4lc (L6)
;;
;; (tree:optimize AST) -> AST, applied by c4lc -O before code
;; generation. Two passes, both things the flat peephole optimizer
;; structurally cannot see:
;;
;;   T1 constant folding: arithmetic on literals, sizeof made literal,
;;      ?: and if/while/for with constant conditions reduced, && / ||
;;      with a constant left side collapsed using c4's EXACT result
;;      semantics (a && b yields b's VALUE when a is truthy; a || b
;;      yields a's VALUE when a is truthy -- neither normalizes to 1).
;;   T2 dead function elimination: functions unreachable from main,
;;      constructors, destructors and taken addresses are dropped.
;;      Identifier references are collected conservatively (a local
;;      shadowing a function name keeps the function alive; that is
;;      only ever too careful). __c4cc_make_va is always kept when
;;      defined, since variadic call sites reach it implicitly.

(begin

;; ---- helpers ----

(define t:reverse (lambda (l) (next t:reverse/2 l (list))))
(define t:reverse/2 (lambda (l acc)
	(if (empty? l) acc (next t:reverse/2 (tail l) (+ (list (head l)) acc)))))
(define t:cons (lambda (x l) (+ (list x) l)))
(define t:second (lambda (l) (index l 1)))
(define t:third (lambda (l) (index l 2)))
(define t:member? (lambda (x l)
	(if (empty? l) false
	(if (= (head l) x) true
	(next t:member? x (tail l))))))

(define t:nfold 0)     ;; folds performed
(define t:ndrop 0)     ;; functions dropped

(define t:num? (lambda (e)
	(if (= 'list (typeof e)) (= (head e) 'num) false)))

;; ---- T1: constant folding ----

;; map over a list of expressions
(define t:fexprs (lambda (l acc)
	(if (empty? l) (t:reverse acc)
	(next t:fexprs (tail l) (t:cons (t:fexpr (head l)) acc)))))

(define t:arith (lambda (op a b)
	(if (= op 'add) (+ a b)
	(if (= op 'sub) (- a b)
	(if (= op 'mul) (* a b)
	(if (= op 'div) (/ a b)
	(if (= op 'mod) (- a (* (/ a b) b))
	(if (= op 'shl) (bit:shl a b)
	(if (= op 'shr) (bit:shr a b)
	(if (= op 'band) (bit:and a b)
	(if (= op 'bor) (bit:or a b)
	(if (= op 'bxor) (bit:xor a b)
	(if (= op 'eq) (if (= a b) 1 0)
	(if (= op 'ne) (if (= a b) 0 1)
	(if (= op 'lt) (if (< a b) 1 0)
	(if (= op 'gt) (if (> a b) 1 0)
	(if (= op 'le) (if (<= a b) 1 0)
	(if (= op 'ge) (if (>= a b) 1 0)
	'nofold))))))))))))))))))

(define t:binops '(add sub mul div mod shl shr band bor bxor
	eq ne lt gt le ge))

(define t:fexpr (lambda (e)
	(begin
		(define h (head e))
		(if (= h 'num) e
		(if (= h 'str) e
		(if (= h 'var) e
		(if (= h 'sizeofa) e
		(if (= h 'sizeof)
			;; make it a literal so it can participate in folds --
			;; except struct types (>= 1024), whose size only the
			;; generator's layout knows
			(if (>= (t:second e) 1024) e
			(begin
				(set! t:nfold (+ t:nfold 1))
				;; word size follows the host, like c4lc-gen's g:WORD
				(list 'num (if (= (t:second e) 0) 1 (sys:wordsize)))))
		(if (= h 'call)
			(t:cons 'call (t:cons (t:second e) (t:fexprs (tail (tail e)) (list))))
		(if (= h 'comma) (t:cons 'comma (t:fexprs (tail e) (list)))
		(if (= h 'assign)
			(list 'assign (t:second e) (t:fexpr (t:third e)))
		(if (= h 'cond) (t:fcond e)
		(if (= h 'land) (t:fland e)
		(if (= h 'lor) (t:flor e)
		(if (= h 'lognot) (t:flognot e)
		(if (= h 'bitnot) (t:fbitnot e)
		(if (= h 'neg) (t:fneg e)
		(if (= h 'cast) (list 'cast (t:second e) (t:fexpr (t:third e)))
		(if (= h 'index)
			(list 'index (t:fexpr (t:second e)) (t:fexpr (t:third e)))
		(if (= h 'deref) (list 'deref (t:fexpr (t:second e)))
		(if (= h 'member) (list 'member (t:fexpr (t:second e)) (t:third e))
		(if (= h 'arrow) (list 'arrow (t:fexpr (t:second e)) (t:third e))
		(if (= h 'addr) e
		(if (= h 'preinc) e
		(if (= h 'predec) e
		(if (= h 'postinc) e
		(if (= h 'postdec) e
		(if (t:member? h t:binops) (t:fbinop e)
		e))))))))))))))))))))))))))))

(define t:fbinop (lambda (e)
	(begin
		(define a (t:fexpr (t:second e)))
		(define b (t:fexpr (t:third e)))
		(if (if (t:num? a) (t:num? b) false)
			;; never fold division by zero; let it fail at runtime
			(if (if (= (t:second b) 0)
					(if (= (head e) 'div) true (= (head e) 'mod))
					false)
				(list (head e) a b)
			(begin
				(define v (t:arith (head e) (t:second a) (t:second b)))
				(if (= v 'nofold) (list (head e) a b)
				(begin
					(set! t:nfold (+ t:nfold 1))
					(list 'num v)))))
		(list (head e) a b)))))

(define t:fcond (lambda (e)
	(begin
		(define c (t:fexpr (t:second e)))
		(if (t:num? c)
			(begin
				(set! t:nfold (+ t:nfold 1))
				(if (= (t:second c) 0)
					(t:fexpr (index e 3))
				(t:fexpr (t:third e))))
		(list 'cond c (t:fexpr (t:third e)) (t:fexpr (index e 3)))))))

;; c4's a && b: BZ over b -- yields b's value when a is truthy, else
;; a's value (0)
(define t:fland (lambda (e)
	(begin
		(define a (t:fexpr (t:second e)))
		(if (t:num? a)
			(begin
				(set! t:nfold (+ t:nfold 1))
				(if (= (t:second a) 0) a (t:fexpr (t:third e))))
		(list 'land a (t:fexpr (t:third e)))))))

;; c4's a || b: BNZ over b -- yields a's value when truthy, else b's
(define t:flor (lambda (e)
	(begin
		(define a (t:fexpr (t:second e)))
		(if (t:num? a)
			(begin
				(set! t:nfold (+ t:nfold 1))
				(if (= (t:second a) 0) (t:fexpr (t:third e)) a))
		(list 'lor a (t:fexpr (t:third e)))))))

(define t:flognot (lambda (e)
	(begin
		(define a (t:fexpr (t:second e)))
		(if (t:num? a)
			(begin
				(set! t:nfold (+ t:nfold 1))
				(list 'num (if (= (t:second a) 0) 1 0)))
		(list 'lognot a)))))

(define t:fbitnot (lambda (e)
	(begin
		(define a (t:fexpr (t:second e)))
		(if (t:num? a)
			(begin
				(set! t:nfold (+ t:nfold 1))
				(list 'num (bit:xor (t:second a) -1)))
		(list 'bitnot a)))))

(define t:fneg (lambda (e)
	(begin
		(define a (t:fexpr (t:second e)))
		(if (t:num? a)
			(begin
				(set! t:nfold (+ t:nfold 1))
				(list 'num (- 0 (t:second a))))
		(list 'neg a)))))

;; ---- T1 statements ----

(define t:fstmts (lambda (l acc)
	(if (empty? l) (t:reverse acc)
	(next t:fstmts (tail l) (t:cons (t:fstmt (head l)) acc)))))

(define t:fstmt (lambda (s)
	(begin
		(define h (head s))
		(if (= h 'block) (t:cons 'block (t:fstmts (tail s) (list)))
		(if (= h 'expr) (list 'expr (t:fexpr (t:second s)))
		(if (= h 'if) (t:fif s)
		(if (= h 'while) (t:fwhile s)
		(if (= h 'dowhile)
			;; the body runs at least once, so only the parts fold
			(list 'dowhile (t:fstmt (t:second s)) (t:fexpr (t:third s)))
		(if (= h 'declstmt) (t:cons 'declstmt (t:fdecls (tail s) (list)))
		(if (= h 'for) (t:ffor s)
		(if (= h 'return)
			(if (= (t:second s) nil) s
				(list 'return (t:fexpr (t:second s))))
		(if (= h 'switch)
			(t:cons 'switch (t:cons (t:fexpr (t:second s))
				(t:fswitch (tail (tail s)) (list))))
		s))))))))))))
;; declaration initializer expressions fold too
(define t:fdecls (lambda (ls acc)
	(if (empty? ls) (t:reverse acc)
	(begin
		(define d (head ls))
		(define init (index d 4))
		(next t:fdecls (tail ls) (t:cons
			(if (if (= init nil) false (= (head init) 'einit))
				(list 'local (t:second d) (t:third d) (index d 3)
					(list 'einit (t:fexpr (t:second init))))
			d)
			acc))))))
(define t:fswitch (lambda (items acc)
	(if (empty? items) (t:reverse acc)
	(begin
		(define it (head items))
		(define h (head it))
		(next t:fswitch (tail items) (t:cons
			(if (= h 'case) it (if (= h 'default) it (t:fstmt it)))
			acc))))))

;; if with a constant condition keeps only the live branch
(define t:fif (lambda (s)
	(begin
		(define c (t:fexpr (t:second s)))
		(if (t:num? c)
			(begin
				(set! t:nfold (+ t:nfold 1))
				(if (= (t:second c) 0)
					(if (= (index s 3) nil) '(empty) (t:fstmt (index s 3)))
				(t:fstmt (t:third s))))
		(list 'if c (t:fstmt (t:third s))
			(if (= (index s 3) nil) nil (t:fstmt (index s 3))))))))

;; while (0) disappears (the body is unreachable; break/continue
;; inside it belong to it, so dropping the whole loop is safe)
(define t:fwhile (lambda (s)
	(begin
		(define c (t:fexpr (t:second s)))
		(if (if (t:num? c) (= (t:second c) 0) false)
			(begin
				(set! t:nfold (+ t:nfold 1))
				'(empty))
		(list 'while c (t:fstmt (t:third s)))))))

(define t:ffor (lambda (s)
	(begin
		(define i (if (= (t:second s) nil) nil (t:fexpr (t:second s))))
		(define c (if (= (t:third s) nil) nil (t:fexpr (t:third s))))
		(if (if (t:num? c) (= (t:second c) 0) false)
			(begin
				;; only the initializer's side effects remain
				(set! t:nfold (+ t:nfold 1))
				(if (= i nil) '(empty) (list 'expr i)))
		(list 'for i c
			(if (= (index s 3) nil) nil (t:fexpr (index s 3)))
			(t:fstmt (index s 4)))))))

;; T1 over declarations (bodies only; initializers are already
;; constants by grammar)
(define t:folddecls (lambda (ds acc)
	(if (empty? ds) (t:reverse acc)
	(begin
		(define d (head ds))
		(next t:folddecls (tail ds) (t:cons
			(if (= (head d) 'func)
				(list 'func (t:second d) (t:third d) (index d 3)
					(index d 4) (index d 5) (index d 6)
					(t:cons 'block (t:fstmts (tail (index d 7)) (list))))
			d)
			acc))))))

;; ---- T2: dead function elimination ----

;; every identifier a function body references (calls, vars, fnaddr)
(define t:refs (lambda (x acc)
	(if (= 'list (typeof x))
		(begin
			(define h (head x))
			(if (= h 'call)
				(t:refs/list (tail (tail x)) (t:cons (t:second x) acc))
			(if (= h 'var) (t:cons (t:second x) acc)
			(if (= h 'fnaddr) (t:cons (t:second x) acc)
			(if (= h 'sizeofa) (t:cons (t:second x) acc)
			(t:refs/list (tail x) acc))))))
	acc)))
(define t:refs/list (lambda (l acc)
	(if (empty? l) acc
	(if (= 'list (typeof l))
		(next t:refs/list (tail l) (t:refs (head l) acc))
	acc))))

;; ((NAME REFS...) ...) for every defined function
(define t:funtable (lambda (ds acc)
	(if (empty? ds) acc
	(begin
		(define d (head ds))
		(next t:funtable (tail ds)
			(if (= (head d) 'func)
				(t:cons (t:cons (t:third d)
					(t:refs (index d 7)
						(t:initrefs (tail (index d 6)) (list))))
					acc)
			acc))))))
;; local initializers can reference functions (&fn, or any expression
;; in an einit)
(define t:initrefs (lambda (ls acc)
	(if (empty? ls) acc
	(begin
		(define init (index (head ls) 4))
		(next t:initrefs (tail ls)
			(if (= init nil) acc
			(if (= (head init) 'fnaddr) (t:cons (t:second init) acc)
			(if (= (head init) 'einit) (t:refs (t:second init) acc)
			acc))))))))

(define t:assoc (lambda (k l)
	(if (empty? l) false
	(if (= (head (head l)) k) (head l)
	(next t:assoc k (tail l))))))

;; roots: main, constructors/destructors, globals' &fn initializers,
;; and __c4cc_make_va (reached implicitly from variadic call sites).
;; In object mode every non-static function is exported, so all of
;; them are roots.
(define tree:objmode false)
(define t:roots (lambda (ds acc)
	(if (empty? ds) acc
	(begin
		(define d (head ds))
		(define h (head d))
		(next t:roots (tail ds)
			(if (= h 'func)
				(if (if tree:objmode (= 0 (bit:and (index d 5) 8)) false)
					(t:cons (t:third d) acc)
				(if (= (t:third d) "main") (t:cons "main" acc)
				(if (= (t:third d) "__c4cc_make_va") (t:cons (t:third d) acc)
				(if (> (bit:and (index d 5) 3) 0) (t:cons (t:third d) acc)
				acc))))
			(if (= h 'global)
				(if (= (index d 5) nil) acc
				(if (= (head (index d 5)) 'fnaddr)
					(t:cons (t:second (index d 5)) acc)
				acc))
			acc)))))))

;; closure over the reference table
(define t:close (lambda (work live table)
	(if (empty? work) live
	(begin
		(define n (head work))
		(if (t:member? n live) (next t:close (tail work) live table)
		(begin
			(define entry (t:assoc n table))
			(next t:close
				(if entry (+ (tail entry) (tail work)) (tail work))
				(t:cons n live)
				table)))))))

(define t:dropdead (lambda (ds live acc)
	(if (empty? ds) (t:reverse acc)
	(begin
		(define d (head ds))
		(if (if (= (head d) 'func)
				(not (t:member? (t:third d) live))
				false)
			(begin
				(set! t:ndrop (+ t:ndrop 1))
				(next t:dropdead (tail ds) live acc))
		(next t:dropdead (tail ds) live (t:cons d acc)))))))


;; ---- T3: inlining small functions (docs/inline-small-functions.md) ----
;;
;; A call in c4 costs JSR, ENT, ADJ and LEV whatever is on the other
;; side of it, plus a push per argument and a bp-relative load for every
;; parameter the body reads. For a one-line accessor that is most of the
;; cost of calling it, and the peephole pass structurally cannot help:
;; a parameter is read with LEA n, which is relative to a frame the
;; callee has and a splice into the caller would not. Here there is no
;; frame yet, so the generator allocates the caller's slots for whatever
;; is spliced in and the problem does not arise.
;;
;; Conservative on purpose. Every rule below exists because breaking it
;; can change what a program means, not because it was easier.

;; Off by default for its first outing: -O's other passes cannot change
;; what a program computes, and this one moves code between functions,
;; so it earns its way in behind a flag until the corpus says otherwise.
(define tree:inlining false)
(define t:ninline 0)      ;; call sites replaced
;; Names inlined at least once. Kept because inlining every call to a
;; function must NOT be what deletes it: another translation unit can
;; still name it at link time, and this pass sees one unit. A static
;; function is the exception -- nothing outside can refer to it, so if
;; T3 took its last caller T2 may have it. That is C's own rule for an
;; inline definition, and it is why these become roots below.
(define t:inlined (list))
;; The budget, in AST nodes. The question is never "is this function
;; small" in the abstract -- it is whether the body is cheaper than the
;; four instructions and N pushes that calling it costs regardless.
(define t:inlinemax 16)

(define t:size (lambda (e)
	(if (= 'list (typeof e)) (next t:size/l e 0) 1)))
(define t:size/l (lambda (l acc)
	(if (empty? l) acc
	(next t:size/l (tail l) (+ acc (t:size (head l)))))))

;; Anything that writes, calls, or steps a variable. A candidate's BODY
;; may be impure -- writing through a pointer is usually the whole point
;; of the function -- but an ARGUMENT may not be, because substitution
;; can duplicate it, drop it, or move it past another argument.
(define t:impure '(assign call preinc predec postinc postdec))
(define t:pure? (lambda (e)
	(if (= 'list (typeof e))
		(if (t:member? (head e) t:impure) false
		(next t:pure?/l (tail e)))
	true)))
(define t:pure?/l (lambda (l)
	(if (empty? l) true
	(if (t:pure? (head l)) (next t:pure?/l (tail l))
	false))))
(define t:pureargs? (lambda (l)
	(if (empty? l) true
	(if (t:pure? (head l)) (next t:pureargs? (tail l))
	false))))

;; Cheap enough to appear twice: a literal or a bare variable. Anything
;; else duplicated is a second memory reference, and two reads of the
;; same place are only the same program while nothing writes between
;; them -- which this pass is in no position to know.
(define t:dupok? (lambda (e)
	(if (= 'list (typeof e))
		(if (= (head e) 'num) true
		(if (= (head e) 'str) true
		(if (= (head e) 'var) true
		false)))
	false)))

;; (var N) occurrences in e
(define t:uses (lambda (n e)
	(if (= 'list (typeof e))
		(if (= (head e) 'var) (if (= (t:second e) n) 1 0)
		(next t:uses/l n (tail e) 0))
	0)))
(define t:uses/l (lambda (n l acc)
	(if (empty? l) acc
	(next t:uses/l n (tail l) (+ acc (t:uses n (head l)))))))

;; Every name whose address is taken, anywhere, at any depth. A function
;; reached through a pointer must still exist to be pointed at.
;; TWO spellings, and missing the second one is a correctness bug rather
;; than a missed optimisation:
;;
;;   (fnaddr N)        -- `&f` in an INITIALISER, which is the only place
;;                        the parser makes this node (c4lc-parse.lisp:631)
;;   (addr (var N))    -- `&f` in an ORDINARY EXPRESSION, which is what
;;                        C4KE writes: install_custom_opcode(OP, &op_halt)
;;
;; Only looking for the first found nothing at all in C4KE -- every one
;; of its opcode handlers is registered by the second spelling -- so the
;; guard that was supposed to keep address-taken functions out of this
;; pass was never keeping anything out.
;;
;; A local or global that merely shares a name with a function will be
;; collected too. That only makes the pass more careful.
(define t:taken (lambda (x acc)
	(if (= 'list (typeof x))
		(if (= (head x) 'fnaddr) (t:cons (t:second x) acc)
		(if (if (= (head x) 'addr)
				(if (= 'list (typeof (t:second x)))
					(= (head (t:second x)) 'var) false)
				false)
			(t:cons (t:second (t:second x)) acc)
		(next t:taken/l (tail x) acc)))
	acc)))
(define t:taken/l (lambda (l acc)
	(if (empty? l) acc
	(if (= 'list (typeof l))
		(next t:taken/l (tail l) (t:taken (head l) acc))
	acc))))
(define t:takenall (lambda (ds acc)
	(if (empty? ds) acc
	(next t:takenall (tail ds) (t:taken (head ds) acc)))))

(define t:bind (lambda (ps as acc)
	(if (empty? ps) (t:reverse acc)
	(next t:bind (tail ps) (tail as)
		(t:cons (list (t:second (head ps)) (head as)) acc)))))

;; Simultaneous substitution. Sequential would be wrong: with
;; f(a,b){return a+b;} the call f(b,1) would substitute a->b first and
;; then have b->1 rewrite the argument it just planted, yielding 1+1.
;; Three node shapes carry a bare identifier, not just `var`:
;;
;;   (call NAME args)  -- c4lc resolves a call name through the ordinary
;;                        symbol table, so a LOCAL holding a function
;;                        pointer is called by name like any other. C4KE
;;                        does exactly that (`int *cb; ... cb();`), and
;;                        renaming the declaration without renaming the
;;                        call is what "undefined identifier: cb" was.
;;   (sizeofa NAME)    -- sizeof applied to an identifier.
;;   (fnaddr NAME)     -- a FUNCTION's address, never a local. Left alone
;;                        deliberately; renaming it would break the link.
;;
;; Only a rename (a binding to a plain `var`) can be applied to those two
;; positions: an arbitrary expression cannot go where an identifier is
;; required, which is why T3 refuses any body that has one.
(define t:rebind (lambda (b n)
	(begin
		(define hit (t:assoc n b))
		(if (= hit false) n
		(begin
			(define v (t:second hit))
			(if (= 'list (typeof v))
				(if (= (head v) 'var) (t:second v) n)
			n))))))

(define t:substb (lambda (b e)
	(if (= 'list (typeof e))
		(if (= (head e) 'var)
			(begin
				(define hit (t:assoc (t:second e) b))
				(if (= hit false) e (t:second hit)))
		(if (= (head e) 'call)
			(t:cons 'call (t:cons (t:rebind b (t:second e))
								  (t:substb/l b (tail (tail e)) (list))))
		(if (= (head e) 'sizeofa)
			(list 'sizeofa (t:rebind b (t:second e)))
		(t:cons (head e) (t:substb/l b (tail e) (list))))))
	e)))
(define t:substb/l (lambda (b l acc)
	(if (empty? l) (t:reverse acc)
	(next t:substb/l b (tail l) (t:cons (t:substb b (head l)) acc)))))

;; ---- who qualifies ----

;; (NAME PARAMS KIND EXPR): KIND is 'value for a (return E) body and
;; 'stmt for an (expr E) one. A void body is not a value, and must only
;; be spliced where a statement was already expected.
(define t:cand (lambda (d taken acc)
	(begin
		(define nm (t:third d))
		(define ps (index d 3))
		(define ss (tail (index d 7)))
		(if tree:inlinereport
			(print ";;     FN" nm "locals" (length (tail (index d 6)))
				   "stmts" (length ss) "calls" (t:ncalls nm)
				   "taken" (if (t:member? nm taken) 1 0)
				   "rets" (t:nret (index d 7))
				   "tailret" (if (t:tailret? ss) 1 0))
		nil)
		(if (index d 4) (t:why nm "variadic" acc)
		(if (= nm "main") acc
		(if (> (bit:and (index d 5) 3) 0) acc    ;; constructor/destructor
		(if (t:member? nm taken) (t:why nm "address taken" acc)
		(if (not (empty? (tail (index d 6)))) (t:why nm "declares locals" acc)
		(if (not (= (length ss) 1)) (t:why nm "multi-statement body" acc)
		(begin
			(define s (head ss))
			(define h (head s))
			(if (= h 'return) (t:candadd nm ps h (t:second s) acc)
			(if (= h 'expr)   (t:candadd nm ps h (t:second s) acc)
			(t:why nm "body is not return/expr" acc)))))))))))))

(define tree:inlinereport false)

;; How many times each function is CALLED in the whole unit. A rule that
;; rejects a function nobody calls costs nothing; one that rejects the
;; function on the hot path costs everything, and only this tells them
;; apart. Built once, consulted by the report.
(define t:calls (list))

;; How many `return`s a body contains, and whether its LAST top-level
;; statement is one. Together these decide whether a body can be spliced
;; as a plain block: no return at all (void, falls off the end) or
;; exactly one that is already last needs no jump. Anything else needs
;; somewhere to jump TO, which the AST has no way to say.
(define t:nret (lambda (x)
	(if (= 'list (typeof x))
		(if (= (head x) 'return) (+ 1 (t:nret/l (tail x)))
		(t:nret/l (tail x)))
	0)))
(define t:nret/l (lambda (l)
	(if (empty? l) 0
	(if (= 'list (typeof l))
		(+ (t:nret (head l)) (t:nret/l (tail l)))
	0))))
(define t:last (lambda (l)
	(if (empty? l) nil
	(if (empty? (tail l)) (head l)
	(next t:last (tail l))))))
(define t:tailret? (lambda (ss)
	(begin
		(define e (t:last ss))
		(if (= e nil) false
		(if (= 'list (typeof e)) (= (head e) 'return) false)))))
(define t:census (lambda (x)
	(if (= 'list (typeof x))
		(begin
			(if (= (head x) 'call) (t:bump (t:second x)) nil)
			(t:census/l (tail x)))
	nil)))
(define t:census/l (lambda (l)
	(if (empty? l) nil
	(if (= 'list (typeof l))
		(begin (t:census (head l)) (next t:census/l (tail l)))
	nil))))
(define t:bump (lambda (nm)
	(begin
		(define e (t:assoc nm t:calls))
		(if (= e false) (set! t:calls (t:cons (list nm 1) t:calls))
			(set! t:calls (t:cons (list nm (+ 1 (t:second e)))
								  t:calls))))))
(define t:ncalls (lambda (nm)
	(begin
		(define e (t:assoc nm t:calls))
		(if (= e false) 0 (t:second e)))))
(define t:censusall (lambda (ds)
	(if (empty? ds) nil
	(begin (t:census (head ds)) (next t:censusall (tail ds))))))
(define t:why (lambda (nm r acc)
	(begin
		(if tree:inlinereport
			(print ";;     no:" nm "-" r "- called" (t:ncalls nm))
		nil)
		acc)))

(define t:candadd (lambda (nm ps h e acc)
	;; The body may WRITE -- storing through a pointer is usually the
	;; point of a function this small -- but it may not CALL. A leaf
	;; makes recursion impossible and keeps one pass from cascading, so
	;; the pass has no fixpoint to reason about.
	(if (= e nil) acc
	(if (t:hassizeofa? e) (t:why nm "body has sizeof(identifier)" acc)
	(if (t:hascall? e) (t:why nm "body calls something" acc)
	(if (> (t:size e) t:inlinemax)
		(t:why nm (+ "too big: " (+ "" (t:size e))) acc)
	(begin
		(if tree:inlinereport
			(print ";;     YES:" nm "size" (t:size e) "- called" (t:ncalls nm))
		nil)
		(t:cons (list nm ps (if (= h 'return) 'value 'stmt) e) acc))))))))

(define t:hassizeofa? (lambda (e)
	(if (= 'list (typeof e))
		(if (= (head e) 'sizeofa) true
		(next t:hassizeofa?/l (tail e)))
	false)))
(define t:hassizeofa?/l (lambda (l)
	(if (empty? l) false
	(if (= 'list (typeof l))
		(if (t:hassizeofa? (head l)) true
		(next t:hassizeofa?/l (tail l)))
	false))))

(define t:hascall? (lambda (e)
	(if (= 'list (typeof e))
		(if (= (head e) 'call) true
		(next t:hascall?/l (tail e)))
	false)))
(define t:hascall?/l (lambda (l)
	(if (empty? l) false
	(if (= 'list (typeof l))
		(if (t:hascall? (head l)) true
		(next t:hascall?/l (tail l)))
	false))))

(define t:cands (lambda (ds taken acc)
	(if (empty? ds) acc
	(begin
		(define d (head ds))
		(next t:cands (tail ds) taken
			(if (= (head d) 'func) (t:cand d taken acc) acc))))))

;; ---- the rewrite ----

(define t:dupsafe? (lambda (ps as body)
	(if (empty? ps) true
	(begin
		(define n (t:second (head ps)))
		(if (> (t:uses n body) 1)
			(if (t:dupok? (head as))
				(next t:dupsafe? (tail ps) (tail as) body)
			false)
		(next t:dupsafe? (tail ps) (tail as) body))))))

;; the spliced expression, or false
(define t:try (lambda (c nm as kind)
	(begin
		(define e (t:assoc nm c))
		(if (= e false) false
		(begin
			(define ps (t:second e))
			(define body (index e 3))
			(if (not (= (t:third e) kind)) false
			(if (not (= (length ps) (length as))) false
			(if (not (t:pureargs? as)) false
			(if (not (t:dupsafe? ps as body)) false
			(begin
				(set! t:ninline (+ t:ninline 1))
				(if (t:member? nm t:inlined) nil
					(set! t:inlined (t:cons nm t:inlined)))
				(t:substb (t:bind ps as (list)) body)))))))))))

(define t:iexpr (lambda (c e)
	(if (= 'list (typeof e))
		(begin
			(define h (head e))
			(if (= h 'call)
				(begin
					(define as (t:iexprs c (tail (tail e)) (list)))
					(define r (t:try c (t:second e) as 'value))
					(if (= r false) (t:cons 'call (t:cons (t:second e) as)) r))
			(t:cons h (t:iexprs c (tail e) (list)))))
	e)))
(define t:iexprs (lambda (c l acc)
	(if (empty? l) (t:reverse acc)
	(next t:iexprs c (tail l) (t:cons (t:iexpr c (head l)) acc)))))

(define t:istmts (lambda (c l acc)
	(if (empty? l) (t:reverse acc)
	(next t:istmts c (tail l) (t:cons (t:istmt c (head l)) acc)))))

(define t:istmt (lambda (c s)
	(begin
		(define h (head s))
		(if (= h 'block) (t:cons 'block (t:istmts c (tail s) (list)))
		(if (= h 'expr)
			;; T4 first: it replaces the whole statement with a block,
			;; where T3 only ever replaces the expression inside it.
			(begin
				(define s4 (t:stmt4 t:c4 s))
				;; A spliced block is NOT walked again. Re-walking it
				;; cascades -- A takes B's body, which still calls C, so
				;; C lands inside A too -- and the growth compounds per
				;; level with nothing bounding it but the call graph
				;; happening to be acyclic.
				(if (= s4 s) (list 'expr (t:istmtexpr c (t:second s)))
					s4))
		(if (= h 'if)
			(list 'if (t:iexpr c (t:second s)) (t:istmt c (t:third s))
				(if (= (index s 3) nil) nil (t:istmt c (index s 3))))
		(if (= h 'while) (list 'while (t:iexpr c (t:second s)) (t:istmt c (t:third s)))
		(if (= h 'dowhile) (list 'dowhile (t:istmt c (t:second s)) (t:iexpr c (t:third s)))
		(if (= h 'declstmt) (t:cons 'declstmt (t:idecls c (tail s) (list)))
		(if (= h 'for)
			(list 'for
				(if (= (t:second s) nil) nil (t:iexpr c (t:second s)))
				(if (= (t:third s) nil) nil (t:iexpr c (t:third s)))
				(if (= (index s 3) nil) nil (t:iexpr c (index s 3)))
				(t:istmt c (index s 4)))
		(if (= h 'return)
			(if (= (t:second s) nil) s (list 'return (t:iexpr c (t:second s))))
		(if (= h 'switch)
			(t:cons 'switch (t:cons (t:iexpr c (t:second s))
				(t:iswitch c (tail (tail s)) (list))))
		s))))))))))))

;; A void candidate splices in HERE and nowhere else: statement position
;; is the only place its body was ever a legal thing to write.
(define t:istmtexpr (lambda (c x)
	(if (= 'list (typeof x))
		(if (= (head x) 'call)
			(begin
				(define as (t:iexprs c (tail (tail x)) (list)))
				(define r (t:try c (t:second x) as 'stmt))
				(if (= r false)
					(begin
						(define v (t:try c (t:second x) as 'value))
						(if (= v false) (t:cons 'call (t:cons (t:second x) as)) v))
				r))
		(t:iexpr c x))
	x)))

(define t:idecls (lambda (c ls acc)
	(if (empty? ls) (t:reverse acc)
	(begin
		(define d (head ls))
		(define init (index d 4))
		(next t:idecls c (tail ls) (t:cons
			(if (if (= init nil) false (= (head init) 'einit))
				(list 'local (t:second d) (t:third d) (index d 3)
					(list 'einit (t:iexpr c (t:second init))))
			d)
			acc))))))

(define t:iswitch (lambda (c items acc)
	(if (empty? items) (t:reverse acc)
	(begin
		(define it (head items))
		(define h (head it))
		(next t:iswitch c (tail items) (t:cons
			(if (= h 'case) it (if (= h 'default) it (t:istmt c it)))
			acc))))))

;; A function whose ADDRESS IS TAKEN must not be spliced INTO, not just
;; refused as a candidate.
;;
;; Splicing changes a function's frame, and therefore the operand of its
;; ENT -- and C4KE READS THAT OPERAND. A custom opcode handler is
;; entered by a route that bypasses its own prologue:
;;
;;     handler = *(custom_opcodes + (ins - CO_BASE));
;;     __c4_adjust(*(handler - 1) * -1);   // the ENT operand
;;     __c4_jmp(handler);                  // jumps PAST the ENT
;;
;; so the stored address, the word before it and the frame it describes
;; are a contract with code somewhere else. The same is true of
;; pm_syscall_handler (`((int *)&fn) + 2 // skip ENT x`). An inliner
;; that grows such a frame is rewriting one side of that contract only,
;; and C4KE's op_* handlers are exactly the small leaf functions T4
;; likes best -- which is how this was found: task argv arriving empty
;; and a stack guard broken 44 bytes in.
;;
;; Taking an address is the only signal available here for "something
;; else knows the shape of this function", so it has to gate both
;; directions.
(define t:inlinedecls (lambda (c taken ds acc)
	(if (empty? ds) (t:reverse acc)
	(begin
		(define d (head ds))
		(next t:inlinedecls c taken (tail ds) (t:cons
			(if (if (= (head d) 'func) (not (t:member? (t:third d) taken)) false)
				(list 'func (t:second d) (t:third d) (index d 3)
					(index d 4) (index d 5) (index d 6)
					(t:cons 'block (t:istmts c (tail (index d 7)) (list))))
			d)
			acc))))))

(define t:inline (lambda (ds)
	(begin
		(set! t:calls (list))
		(if tree:inlinereport (t:censusall ds) nil)
		(define taken (t:takenall ds (list)))
		(define c (t:cands ds taken (list)))
		(set! t:c4 (t:cands4 ds taken (list)))
		(if (if (empty? c) (empty? t:c4) false) ds
			(t:inlinedecls c taken ds (list))))))

;; An inlined function stays reachable unless it is static: T3 having
;; taken its last CALL says nothing about whether another unit names it.
;; Adding it to T2's roots is the whole mechanism -- T2 already keeps
;; every non-static function in object mode, so this only matters for a
;; whole-program build, which is exactly where the call sites vanished.
(define t:static? (lambda (d) (> (bit:and (index d 5) 8) 0)))
(define t:inlineroots (lambda (ds acc)
	(if (empty? ds) acc
	(begin
		(define d (head ds))
		(next t:inlineroots (tail ds)
			(if (= (head d) 'func)
				(if (t:member? (t:third d) t:inlined)
					(if (t:static? d) acc (t:cons (t:third d) acc))
				acc)
			acc))))))

;; ---- T4: splicing a whole body (docs/inline-small-functions.md) ------
;;
;; T3 substitutes an expression into an expression. T4 splices a BLOCK
;; into statement position, which is what the kernels are made of: a
;; couple of statements, a local or two, and a return at the end.
;;
;; Two things make this safer than T3 rather than more dangerous.
;;
;; A parameter becomes a real LOCAL initialised with the argument, so
;; every argument is evaluated exactly once, left to right, exactly as
;; the call would have. T3's purity and duplication rules exist only
;; because it substitutes an argument's TEXT; none of them are needed
;; here, and an argument may call, assign and increment freely.
;;
;; And the splice is a `block`, which c4lc-gen already gives its own
;; scope (g:syms is saved and restored across one) with its own frame
;; slots (g:scanstmt pre-counts declstmt words so ENT reserves them).
;; Names are still made unique per splice, because a declaration's
;; initialiser is generated with the name already in scope -- so an
;; un-renamed `int x = x` would read itself rather than the caller's.

(define t:c4 (list))        ;; the T4 candidate table for this unit
(define t:uid 0)
(define t:n4 0)             ;; bodies spliced
(define t:n4b 0)            ;; ...of which needed an exit label
;; Bigger than T3's budget on purpose: a body with statements in it is
;; not an accessor, and the call it replaces still costs the same four
;; instructions plus a push per argument.
(define t:inline4max 60)

;; Control must not be able to leave the body early, because a block has
;; nowhere to leave TO. Either it never returns (void, falling off the
;; end) or its single return is already the last thing it does.
(define t:splicable? (lambda (d ss)
	(begin
		(define n (t:nret (index d 7)))
		(if (= n 0) true
		(if (= n 1) (t:tailret? ss) false)))))

(define t:selfcalls? (lambda (nm x)
	(if (= 'list (typeof x))
		(if (if (= (head x) 'call) (= (t:second x) nm) false) true
		(next t:selfcalls?/l nm (tail x)))
	false)))
(define t:selfcalls?/l (lambda (nm l)
	(if (empty? l) false
	(if (= 'list (typeof l))
		(if (t:selfcalls? nm (head l)) true
		(next t:selfcalls?/l nm (tail l)))
	false))))

;; (NAME PARAMS LOCALS STMTS NRETS)
(define t:cand4 (lambda (d taken acc)
	(begin
		(define nm (t:third d))
		(define ss (tail (index d 7)))
		(if (index d 4) acc
		(if (= nm "main") acc
		(if (> (bit:and (index d 5) 3) 0) acc
		(if (t:member? nm taken) acc
		(if (empty? ss) acc
		(if (t:selfcalls? nm (index d 7)) (t:why nm "recursive" acc)
		(if (> (t:size (index d 7)) t:inline4max)
			(t:why nm (+ "body too big: " (+ "" (t:size (index d 7)))) acc)
		(begin
			(if tree:inlinereport
				(print ";;     T4:" nm "size" (t:size (index d 7))
					   "- called" (t:ncalls nm))
			nil)
			(t:cons (list nm (index d 3) (tail (index d 6)) ss
						  (t:nret (index d 7))
						  (t:splicable? d ss)
						  (t:allvalued? (index d 7))) acc))))))))))))

(define t:cands4 (lambda (ds taken acc)
	(if (empty? ds) acc
	(begin
		(define d (head ds))
		(next t:cands4 (tail ds) taken
			(if (= (head d) 'func) (t:cand4 d taken acc) acc))))))

;; ---- renaming ----

(define t:fresh (lambda (n) (+ (+ "__il" (+ "" t:uid)) (+ "_" n))))

;; A parameter does NOT have to become a local.
;;
;; Measured: making every parameter a local made C4KE 8% SLOWER, and the
;; arithmetic says why. A call argument is one PSH -- the value is
;; already where the callee wants it. Binding it to a local instead is
;; `LEA slot; PSH; <arg>; SI`, three instructions more, per parameter,
;; per site. Against the four a call costs (JSR, ENT, ADJ, LEV) two
;; parameters already lose, and the frame grows on top.
;;
;; So a parameter is bound to a local only when it has to be:
;;   - the argument is not a literal or a plain variable, so substituting
;;     it could duplicate work or read a different value later;
;;   - the body ASSIGNS to the parameter, or takes its address;
;;   - the parameter appears where an identifier is required (a call
;;     name, sizeof), which only a name can fill.
;; Otherwise the argument is substituted and the parameter costs nothing.
(define t:mutators '(assign preinc predec postinc postdec))
(define t:isvar? (lambda (n e)
	(if (= 'list (typeof e))
		(if (= (head e) 'var) (= (t:second e) n) false)
	false)))
(define t:assigned? (lambda (n x)
	(if (= 'list (typeof x))
		(if (if (t:member? (head x) t:mutators) (t:isvar? n (t:second x)) false)
			true
		(if (if (= (head x) 'addr) (t:isvar? n (t:second x)) false) true
		(next t:assigned?/l n (tail x))))
	false)))
(define t:assigned?/l (lambda (n l)
	(if (empty? l) false
	(if (= 'list (typeof l))
		(if (t:assigned? n (head l)) true
		(next t:assigned?/l n (tail l)))
	false))))
(define t:idpos? (lambda (n x)
	(if (= 'list (typeof x))
		(if (if (= (head x) 'call) (= (t:second x) n) false) true
		(if (if (= (head x) 'sizeofa) (= (t:second x) n) false) true
		(next t:idpos?/l n (tail x))))
	false)))
(define t:idpos?/l (lambda (n l)
	(if (empty? l) false
	(if (= 'list (typeof l))
		(if (t:idpos? n (head l)) true
		(next t:idpos?/l n (tail l)))
	false))))

(define t:needslocal? (lambda (p a body)
	(begin
		(define n (t:second p))
		(if (not (t:dupok? a)) true
		(if (t:assigned? n body) true
		(if (t:idpos? n body) true
		false))))))

;; bindings: substituted parameters map to their ARGUMENT, bound ones to
;; the fresh local that will hold it
(define t:pbind (lambda (ps as body acc)
	(if (empty? ps) acc
	(begin
		(define p (head ps))
		(define n (t:second p))
		(next t:pbind (tail ps) (tail as) body
			(t:cons (list n (if (t:needslocal? p (head as) body)
								(list 'var (t:fresh n))
							(head as)))
				acc))))))

(define t:pdecls2 (lambda (ps as body acc)
	(if (empty? ps) (t:reverse acc)
	(begin
		(define p (head ps))
		(next t:pdecls2 (tail ps) (tail as) body
			(if (t:needslocal? p (head as) body)
				(t:cons (list 'local (head p) (t:fresh (t:second p)) nil
							  (list 'einit (head as)))
					acc)
			acc))))))

(define t:renmap/p (lambda (ps acc)
	(if (empty? ps) acc
	(next t:renmap/p (tail ps)
		(t:cons (list (t:second (head ps))
					  (list 'var (t:fresh (t:second (head ps))))) acc)))))
(define t:renmap/l (lambda (ls acc)
	(if (empty? ls) acc
	(next t:renmap/l (tail ls)
		(t:cons (list (t:third (head ls))
					  (list 'var (t:fresh (t:third (head ls))))) acc)))))
(define t:renmap (lambda (ps ls) (t:renmap/l ls (t:renmap/p ps (list)))))

;; A parameter becomes `TYPE fresh = ARG;`. This is where every argument
;; is evaluated, once, in order.
(define t:pdecls (lambda (ps as acc)
	(if (empty? ps) (t:reverse acc)
	(next t:pdecls (tail ps) (tail as)
		(t:cons (list 'local (head (head ps))
					  (t:fresh (t:second (head ps))) nil
					  (list 'einit (head as)))
			acc)))))

(define t:ldecls (lambda (ls b acc)
	(if (empty? ls) (t:reverse acc)
	(begin
		(define d (head ls))
		(next t:ldecls (tail ls) b
			(t:cons (list 'local (t:second d) (t:fresh (t:third d))
						  (index d 3)
						  (if (= (index d 4) nil) nil (t:substb b (index d 4))))
				acc))))))

;; ---- T4b: bodies that return early ----------------------------------
;;
;; A `return` in the middle of a body has to go somewhere, and in the
;; callee that somewhere was LEV. Spliced, it becomes a jump to the end
;; of the block -- the same shape `break` already has, lowered by
;; c4lc-gen through the same JMP-to-a-label. The tree pass cannot name a
;; generator label, so it names its own SPLICE and the generator makes
;; the mapping on first mention (the jump comes first; it is forward).
;;
;; This is what needed a `goto` and does not get one: `ijmp`/`ilabel`
;; are nodes only T4 creates and only the generator reads. The C dialect
;; is untouched.

;; A value is wanted, so every return must have one. A body that mixes
;; `return x;` and a bare `return;` has nothing to give on one path, and
;; guessing is worse than declining.
(define t:allvalued? (lambda (x)
	(if (= 'list (typeof x))
		(if (= (head x) 'return) (if (= (t:second x) nil) false true)
		(next t:allvalued?/l (tail x)))
	true)))
(define t:allvalued?/l (lambda (l)
	(if (empty? l) true
	(if (= 'list (typeof l))
		(if (t:allvalued? (head l)) (next t:allvalued?/l (tail l)) false)
	true))))

(define t:retjmp (lambda (s lhs id)
	(begin
		(define e (t:second s))
		(if (= lhs nil)
			(if (= e nil) (list 'ijmp id)
				(list 'block (list 'expr e) (list 'ijmp id)))
		(list 'block (list 'expr (list 'assign lhs e)) (list 'ijmp id))))))

;; Every return, at every depth. A return inside one of the body's own
;; loops jumps clear of it, which is what returning did.
(define t:retmap (lambda (s lhs id)
	(begin
		(define h (head s))
		(if (= h 'return) (t:retjmp s lhs id)
		(if (= h 'block) (t:cons 'block (t:retmaps (tail s) lhs id (list)))
		(if (= h 'if)
			(list 'if (t:second s) (t:retmap (t:third s) lhs id)
				(if (= (index s 3) nil) nil (t:retmap (index s 3) lhs id)))
		(if (= h 'while)
			(list 'while (t:second s) (t:retmap (t:third s) lhs id))
		(if (= h 'dowhile)
			(list 'dowhile (t:retmap (t:second s) lhs id) (t:third s))
		(if (= h 'for)
			(list 'for (t:second s) (t:third s) (index s 3)
				  (t:retmap (index s 4) lhs id))
		(if (= h 'switch)
			(t:cons 'switch (t:cons (t:second s)
				(t:retswitch (tail (tail s)) lhs id (list))))
		s))))))))))
(define t:retmaps (lambda (l lhs id acc)
	(if (empty? l) (t:reverse acc)
	(next t:retmaps (tail l) lhs id (t:cons (t:retmap (head l) lhs id) acc)))))
(define t:retswitch (lambda (items lhs id acc)
	(if (empty? items) (t:reverse acc)
	(begin
		(define it (head items))
		(define h (head it))
		(next t:retswitch (tail items) lhs id (t:cons
			(if (= h 'case) it (if (= h 'default) it (t:retmap it lhs id)))
			acc))))))

;; id 0 means the body needs no exit label: its only return, if it has
;; one, is already the last thing it does, so it can simply fall out of
;; the block. That is the common case and it costs nothing.
(define t:body4 (lambda (ss b lhs id acc)
	(if (empty? ss) (t:reverse acc)
	(begin
		(define s (t:substb b (head ss)))
		(next t:body4 (tail ss) b lhs id
			(t:cons (if (= id 0)
						(if (if (= 'list (typeof s)) (= (head s) 'return) false)
							(t:retjmp2 s lhs)
						s)
					(t:retmap s lhs id))
				acc))))))

;; the trailing-return case, where there is nowhere to jump to and no
;; need: the value lands and the block ends
(define t:retjmp2 (lambda (s lhs)
	(begin
		(define e (t:second s))
		(if (= lhs nil)
			(if (= e nil) '(empty) (list 'expr e))
		(list 'expr (list 'assign lhs e))))))

(define t:splice4 (lambda (e as lhs)
	(begin
		(set! t:uid (+ t:uid 1))
		(define ps (t:second e))
		(define ls (t:third e))
		(define ss (index e 3))
		(define bodyn (t:cons 'block ss))
		(define b (t:renmap/l ls (t:pbind ps as bodyn (list))))
		(define pd (t:pdecls2 ps as bodyn (list)))
		(define ld (t:ldecls ls b (list)))
		;; entry: 0 name 1 params 2 locals 3 stmts 4 nrets
		;;        5 needs-no-exit-label  6 every-return-has-a-value
		(define id (if (index e 5) 0 t:uid))
		(set! t:n4 (+ t:n4 1))
		(if (= id 0) nil (set! t:n4b (+ t:n4b 1)))
		(if (t:member? (head e) t:inlined) nil
			(set! t:inlined (t:cons (head e) t:inlined)))
		(t:cons 'block
			(+ (if (empty? pd) (list) (list (t:cons 'declstmt pd)))
			(+ (if (empty? ld) (list) (list (t:cons 'declstmt ld)))
			(+ (t:body4 ss b lhs id (list))
			   (if (= id 0) (list) (list (list 'ilabel id))))))))))

;; A candidate whose value is wanted must have something to give.
(define t:try4 (lambda (c nm as lhs)
	(begin
		(define e (t:assoc nm c))
		(if (= e false) false
		(if (not (= (length (t:second e)) (length as))) false
		;; a value was asked for and some path has none to give
		(if (if (= lhs nil) false (not (index e 6))) false
		(t:splice4 e as lhs)))))))

(define t:call? (lambda (x)
	(if (= 'list (typeof x)) (= (head x) 'call) false)))

;; The two statement shapes worth taking. `f(a);` throws the value away;
;; `x = f(a);` wants it, and the trailing return is where it comes from.
;; A more interesting lvalue than a plain variable is left alone: its
;; address would then be computed after the body ran instead of before.
(define t:stmt4 (lambda (c s)
	(begin
		(define x (t:second s))
		(if (t:call? x)
			(begin
				(define r (t:try4 c (t:second x) (tail (tail x)) nil))
				(if (= r false) s r))
		(if (if (= 'list (typeof x)) (= (head x) 'assign) false)
			(begin
				(define lv (t:second x))
				(define rv (t:third x))
				(if (if (t:call? rv)
						(if (= 'list (typeof lv)) (= (head lv) 'var) false)
						false)
					(begin
						(define r (t:try4 c (t:second rv) (tail (tail rv)) lv))
						(if (= r false) s r))
				s))
		s)))))

;; ---- entry ----

;; Fold, inline, fold again, then drop what is dead.
;;
;; The second fold is not tidiness: inlining plants arguments into a
;; body, so f(2) against `int f(int x){return x*3;}` becomes 2*3, which
;; is only a literal if something folds it afterwards.
;;
;; Dropping dead functions runs LAST for the same reason it is worth
;; running at all here: a function whose every call site was inlined has
;; no callers left, and T2 then deletes the function itself. The two
;; passes compound, and that ordering is why T3 sits where it does.
(define tree:optimize (lambda (ast)
	(begin
		(set! t:nfold 0)
		(set! t:ndrop 0)
		(set! t:ninline 0)
		(set! t:n4 0)
		(set! t:n4b 0)
		(set! t:uid 0)
		(set! t:c4 (list))
		(set! t:inlined (list))
		(define ds0 (t:folddecls (tail ast) (list)))
		(define ds (if tree:inlining (t:folddecls (t:inline ds0) (list)) ds0))
		(define table (t:funtable ds (list)))
		(define live (t:close (t:inlineroots ds (t:roots ds (list))) (list) table))
		(define ds2 (t:dropdead ds live (list)))
		(print ";;   tree: folded" t:nfold "- inlined" t:ninline
			   "- spliced" t:n4 "(" t:n4b "with an exit label)"
			   "- dropped" t:ndrop "functions")
		(t:cons 'program ds2))))

)
