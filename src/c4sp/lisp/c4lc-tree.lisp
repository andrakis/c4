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
				(list 'num (if (= (t:second e) 0) 1 8))))
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
;; and __c4cc_make_va (reached implicitly from variadic call sites)
(define t:roots (lambda (ds acc)
	(if (empty? ds) acc
	(begin
		(define d (head ds))
		(define h (head d))
		(next t:roots (tail ds)
			(if (= h 'func)
				(if (= (t:third d) "main") (t:cons "main" acc)
				(if (= (t:third d) "__c4cc_make_va") (t:cons (t:third d) acc)
				(if (> (bit:and (index d 5) 3) 0) (t:cons (t:third d) acc)
				acc)))
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

;; ---- entry ----

(define tree:optimize (lambda (ast)
	(begin
		(set! t:nfold 0)
		(set! t:ndrop 0)
		(define ds (t:folddecls (tail ast) (list)))
		(define table (t:funtable ds (list)))
		(define live (t:close (t:roots ds (list)) (list) table))
		(define ds2 (t:dropdead ds live (list)))
		(print ";;   tree: folded" t:nfold "- dropped" t:ndrop "functions")
		(t:cons 'program ds2))))

)
