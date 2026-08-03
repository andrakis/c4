;; c4opt.lisp -- peephole optimizer for .c4r instruction lists (M6)
;;
;; Operates on the labelled instruction lists c4r.lisp produces. The whole
;; argument for c4sp (design 1): each pass here is a few lines of list
;; manipulation that would be a page of C4.
;;
;; Passes, from design 9.4:
;;   fold      IMM a; PSH; IMM b; OP  ->  IMM (a OP b)   (both plain ints)
;;   shl       PSH; IMM 8; MUL        ->  PSH; IMM 3; SHL
;;   adj0      ADJ 0                  ->  (removed)
;;   jmpnext   JMP L just before (label L)               (falls through)
;;   thread    JMP/BZ/BNZ -> L1 where L1: JMP L2  ->  target L2
;;   dead      anything after JMP/LEV up to the next label
;;
;; Safety rests on two facts: every reachable basic block starts at a
;; label (its address must appear in a patch, and every patch target
;; became a label when decoding), and label pseudo-instructions are never
;; removed, so entry/symbol/constructor references stay valid. A pattern
;; window never crosses a label. Raw (word W) items act as barriers.
;;
;; Requires c4r.lisp to be loaded first (for W and the op tables).

(begin

;; run counters, reset by c4opt:optimize
(define opt:n-fold 0)
(define opt:n-shl 0)
(define opt:n-adj0 0)
(define opt:n-jmpnext 0)
(define opt:n-thread 0)
(define opt:n-dead 0)
(define opt:n-tail 0)

;; ---- helpers ----

;; is instruction I exactly (OP) or (OP plain-int-operand)?
(define opt:is (lambda (I Name)
	(if (= Name (head I)) true false)))

;; (IMM 5) with a plain integer operand (never a code/data/extern ref)
(define opt:plain (lambda (I Name)
	(if (= Name (head I))
		(if (= 2 (length I)) (= 'number (typeof (second I))) false)
		false)))

(define opt:is-label (lambda (I) (= 'label (head I))))

;; the code-label id a JMP/BZ/BNZ targets, or nil
(define opt:jump-target (lambda (I)
	(if (= 2 (length I))
		(if (= 'list (typeof (second I)))
			(if (= 'code (head (second I))) (second (second I)) nil)
			nil)
		nil)))

;; truncated modulo, matching the VM's MOD on negatives
(define opt:mod (lambda (a b) (- a (* (/ a b) b))))

;; The stack operand is on the LEFT: a = *sp++ OP a
(define opt:fold1 (lambda (Op a b)
	(if (= 'ADD Op) (+ a b)
	(if (= 'SUB Op) (- a b)
	(if (= 'MUL Op) (* a b)
	(if (= 'DIV Op) (if (= 0 b) nil (/ a b))
	(if (= 'MOD Op) (if (= 0 b) nil (opt:mod a b))
	(if (= 'AND Op) (bit:and a b)
	(if (= 'OR Op) (bit:or a b)
	(if (= 'XOR Op) (bit:xor a b)
	(if (= 'SHL Op) (bit:shl a b)
	(if (= 'SHR Op) (bit:shr a b)
	(if (= 'EQ Op) (if (= a b) 1 0)
	(if (= 'NE Op) (if (= a b) 0 1)
	(if (= 'LT Op) (if (< a b) 1 0)
	(if (= 'GT Op) (if (> a b) 1 0)
	(if (= 'LE Op) (if (<= a b) 1 0)
	(if (= 'GE Op) (if (>= a b) 1 0)
	'nofold))))))))))))))))))

;; ---- passes; each takes and returns an instruction list ----

;; constant folding: the window is (IMM a) (PSH) (IMM b) (OP).
;; opt:fold-try returns the folded (IMM v), or nil.
(define opt:fold-try (lambda (I R)
	(if (not (opt:plain I 'IMM)) nil
	(if (< (length R) 3) nil
	(if (not (opt:is (head R) 'PSH)) nil
	(if (not (opt:plain (second R) 'IMM)) nil
	(if (not (= 1 (length (third R)))) nil
	(opt:fold-value (opt:fold1 (head (third R))
		(second I) (second (second R)))))))))))
(define opt:fold-value (lambda (V)
	(if (= 'nofold V) nil
		(if (= nil V) nil
			(list 'IMM V)))))
(define opt:fold (lambda (Code) (next opt:fold/2 Code (list))))
(define opt:fold/2 (lambda (Code Acc) (begin
	(if (empty? Code) (reverse Acc) (begin
		(define F (opt:fold-try (head Code) (tail Code)))
		(if (= nil F)
			(next opt:fold/2 (tail Code) (cons (head Code) Acc))
			(begin
				(set! opt:n-fold (+ 3 opt:n-fold))
				;; push the folded IMM back onto the input: it may fold
				;; again with what follows
				(next opt:fold/2
					(cons F (tail (tail (tail (tail Code))))) Acc)))))
)))

;; PSH; IMM 8; MUL -> PSH; IMM 3; SHL (every pointer subscript emits this)
(define opt:shl-try (lambda (I R)
	(if (not (opt:is I 'PSH)) false
	(if (< (length R) 2) false
	(if (not (opt:plain (head R) 'IMM)) false
	(if (not (= 8 (second (head R)))) false
	(if (not (= 1 (length (second R)))) false
	(opt:is (second R) 'MUL))))))))
(define opt:shl (lambda (Code) (next opt:shl/2 Code (list))))
(define opt:shl/2 (lambda (Code Acc)
	(if (empty? Code) (reverse Acc)
		(if (opt:shl-try (head Code) (tail Code))
			(begin
				(set! opt:n-shl (+ 1 opt:n-shl))
				;; Acc is reversed: PSH, then IMM 3, then SHL
				(next opt:shl/2 (tail (tail (tail Code)))
					(cons (list 'SHL)
						(cons (list 'IMM 3) (cons (head Code) Acc)))))
			(next opt:shl/2 (tail Code) (cons (head Code) Acc))))))

;; ADJ 0 does nothing (emitted for every zero-argument call).
;; NOTE the shape of every removal pass: (next ...) is a call, not a
;; control transfer, so each path through the body must end in exactly
;; one tail call -- an "early" next inside a begin would run and then
;; fall through to the second one.
(define opt:adj0 (lambda (Code) (next opt:adj0/2 Code (list))))
(define opt:adj0/2 (lambda (Code Acc)
	(if (empty? Code) (reverse Acc)
		(if (if (opt:plain (head Code) 'ADJ) (= 0 (second (head Code))) false)
			(begin
				(set! opt:n-adj0 (+ 1 opt:n-adj0))
				(next opt:adj0/2 (tail Code) Acc))
			(next opt:adj0/2 (tail Code) (cons (head Code) Acc))))))

;; JMP to a label reached by falling through anyway
(define opt:jmpnext (lambda (Code) (next opt:jmpnext/2 Code (list))))
(define opt:jmpnext/2 (lambda (Code Acc)
	(if (empty? Code) (reverse Acc)
		(if (opt:drop-jmp (head Code) (tail Code))
			(begin
				(set! opt:n-jmpnext (+ 1 opt:n-jmpnext))
				(next opt:jmpnext/2 (tail Code) Acc))
			(next opt:jmpnext/2 (tail Code) (cons (head Code) Acc))))))
(define opt:drop-jmp (lambda (I R)
	(if (opt:is I 'JMP)
		(if (= nil (opt:jump-target I)) false
			(opt:label-follows (opt:jump-target I) R))
		false)))
;; is (label T) among the immediately following label pseudo-instructions?
(define opt:label-follows (lambda (T Code)
	(if (empty? Code) false
		(if (opt:is-label (head Code))
			(if (= T (second (head Code))) true
				(next opt:label-follows T (tail Code)))
			false))))

;; jump threading: a jump to a label whose only content is JMP L2 can go
;; to L2 directly. The label -> forwarded-target map is a word array
;; indexed by label id (label ids are original word offsets, so bounded).
(define opt:thread (lambda (Code) (begin
	(define MaxL (c4r:max-label Code 0))
	(define FT (string:alloc (* W (+ MaxL 2))))    ;; 0 = no forwarding
	(next opt:thread-scan Code Code FT))))
;; record, for every label, the target of an immediately following JMP
(define opt:thread-scan (lambda (Scan Code FT) (begin
	(if (empty? Scan)
		(next opt:thread/2 Code (list) FT)
		(begin
			(if (opt:is-label (head Scan)) (begin
				(define T (opt:thread-following (tail Scan)))
				(if (not (= nil T))
					;; store T+1 so 0 keeps meaning "none"
					(string:word! FT (* W (second (head Scan))) (+ 1 T)))))
			(next opt:thread-scan (tail Scan) Code FT)))
)))
;; the JMP target directly after a label (skipping further labels)
(define opt:thread-following (lambda (Code)
	(if (empty? Code) nil
		(if (opt:is-label (head Code))
			(next opt:thread-following (tail Code))
			(if (opt:is (head Code) 'JMP) (opt:jump-target (head Code)) nil)))))
(define opt:thread/2 (lambda (Code Acc FT) (begin
	(if (empty? Code) (reverse Acc) (begin
		(define I (head Code))
		(if (if (opt:is I 'JMP) true (if (opt:is I 'BZ) true (opt:is I 'BNZ))) (begin
			(define T (opt:jump-target I))
			(if (not (= nil T)) (begin
				(define F (string:word FT (* W T)))
				(if (> F 0)
					(if (not (= (- F 1) T)) (begin  ;; ignore self loops
						(set! opt:n-thread (+ 1 opt:n-thread))
						(set! I (list (head I) (list 'code (- F 1)))))))))))
		(next opt:thread/2 (tail Code) (cons I Acc) FT)))
)))

;; tail calls (design 9.4, "needs frame checking, do last"): a zero-arg
;; call in tail position -- JSR f directly followed by LEV -- can reuse
;; the caller's frame. The naive rewrite in the design table (ADJ n before
;; the jump) would discard the callee's arguments, so only the zero-arg
;; form is transformed, and precisely:
;;
;;     JSR f; LEV   ->   ADJ (m - k); JMP f+2
;;
;; where m is the enclosing function's ENT operand, k the callee's, and
;; f+2 enters the callee just past its ENT. The ADJ moves sp from bp-m to
;; bp-k; bp is untouched, so the callee's locals occupy our frame and its
;; LEV pops OUR saved (bp, retpc) pair -- returning straight to our
;; caller. Tail recursion becomes O(1) stack. When m = k the ADJ is 0 and
;; the next round's adj0 pass deletes it.
;;
;; Guards: the callee label must be immediately followed by ENT (else it
;; is not a function entry we understand), and variadic callees
;; (ATTR_VARIADIC, 0x20, from the symbol table) are excluded -- their
;; argument-slot reads would land in the caller's frame.
;;
;; "f+2" needs a label after the callee's ENT; fresh label ids are
;; allocated past the current maximum.

(define opt:tail (lambda (Code Syms) (begin
	(define MaxL (c4r:max-label Code 0))
	(define EM (string:alloc (* W (+ MaxL 2))))  ;; label -> ENT operand + 1
	(define SM (string:alloc (* W (+ MaxL 2))))  ;; label -> skip label + 1
	(define BAN (string:alloc (+ MaxL 2)))       ;; byte: 1 = variadic
	(opt:tail-ban Syms BAN MaxL)
	(opt:tail-entmap Code EM)
	(opt:tail-mark Code EM SM BAN (+ MaxL 1))
	(next opt:tail/2 Code (list) EM SM 0 0))))

;; mark variadic function symbols' entry labels as untouchable
(define opt:tail-ban (lambda (Syms BAN MaxL)
	(if (empty? Syms) nil (begin
		(define S (head Syms))
		(if (opt:tail-ban? S MaxL) (string:byte! BAN (second (index S 5)) 1))
		(next opt:tail-ban (tail Syms) BAN MaxL)))))
(define opt:tail-ban? (lambda (S MaxL)
	(if (= 129 (third S))
		(if (= 'list (typeof (index S 5)))
			(if (<= (second (index S 5)) MaxL)
				(> (bit:and 32 (index S 3)) 0)
				false)
			false)
		false)))

;; label -> the operand of an ENT immediately following it
(define opt:tail-entmap (lambda (Code EM)
	(if (empty? Code) nil (begin
		(if (opt:is-label (head Code))
			(if (not (empty? (tail Code)))
				(if (opt:plain (second Code) 'ENT)
					(string:word! EM (* W (second (head Code)))
						(+ 1 (second (second Code)))))))
		(next opt:tail-entmap (tail Code) EM)))))

;; is this instruction pair a transformable tail call to label F?
(define opt:tail-site (lambda (I R EM SM BAN)
	(if (not (opt:is I 'JSR)) nil
	(if (empty? R) nil
	(if (not (= 1 (length (head R)))) nil
	(if (not (opt:is (head R) 'LEV)) nil
	(opt:tail-site2 (opt:jump-target I) EM SM BAN)))))))
(define opt:tail-site2 (lambda (F EM SM BAN)
	(if (= nil F) nil
	(if (= 0 (string:word EM (* W F))) nil
	(if (= 1 (string:byte BAN F)) nil
	F)))))

;; give every tail-called function a fresh skip label id in SM
(define opt:tail-mark (lambda (Code EM SM BAN NextId) (begin
	(if (empty? Code) NextId (begin
		(define F (opt:tail-site (head Code) (tail Code) EM SM BAN))
		(if (= nil F) nil
			(if (= 0 (string:word SM (* W F))) (begin
				(string:word! SM (* W F) (+ 1 NextId))
				(set! NextId (+ 1 NextId)))))
		(next opt:tail-mark (tail Code) EM SM BAN NextId))))))

;; does the walk transform here? Only sites whose callee got a skip label.
(define opt:tail-go (lambda (I R SM)
	(if (not (opt:is I 'JSR)) nil
	(if (empty? R) nil
	(if (not (= 1 (length (head R)))) nil
	(if (not (opt:is (head R) 'LEV)) nil
	(opt:tail-go2 (opt:jump-target I) SM)))))))
(define opt:tail-go2 (lambda (F SM)
	(if (= nil F) nil
		(if (> (string:word SM (* W F)) 0) F nil))))

;; the rewrite walk. CurM = enclosing ENT operand + 1 (0 = unknown);
;; Pending = skip label id + 1 to insert after the next ENT.
(define opt:tail/2 (lambda (Code Acc EM SM CurM Pending) (begin
	(if (empty? Code) (reverse Acc) (begin
		(define I (head Code))
		(define F (opt:tail-go I (tail Code) SM))
		(if (if (= nil F) false (> CurM 0))
			;; transform: consume JSR and LEV, emit ADJ (m-k); JMP skip
			(begin
				(set! opt:n-tail (+ 1 opt:n-tail))
				(next opt:tail/2 (tail (tail Code))
					(cons (list 'JMP (list 'code (- (string:word SM (* W F)) 1)))
						(cons (list 'ADJ (- (- CurM 1) (- (string:word EM (* W F)) 1)))
							Acc))
					EM SM CurM Pending))
			(begin
				(if (opt:is-label I)
					(if (> (string:word SM (* W (second I))) 0)
						(set! Pending (string:word SM (* W (second I))))))
				(if (opt:plain I 'ENT)
					(set! CurM (+ 1 (second I))))
				(if (if (opt:plain I 'ENT) (> Pending 0) false)
					;; emit ENT then the skip label
					(next opt:tail/2 (tail Code)
						(cons (list 'label (- Pending 1)) (cons I Acc))
						EM SM CurM 0)
					(next opt:tail/2 (tail Code) (cons I Acc) EM SM CurM Pending))))))
)))

;; dead code: after an unconditional JMP or LEV, nothing can execute until
;; the next label. Raw (word W) items are kept as barriers.
(define opt:dead (lambda (Code) (next opt:dead/2 Code (list) false)))
(define opt:dead/2 (lambda (Code Acc Dropping) (begin
	(if (empty? Code) (reverse Acc) (begin
		(define I (head Code))
		;; labels, raw words and patched table words end a dead region
		(if (opt:is-label I) (set! Dropping false))
		(if (= 'word (head I)) (set! Dropping false))
		(if (= 'cword (head I)) (set! Dropping false))
		(if Dropping
			(begin
				(set! opt:n-dead (+ 1 opt:n-dead))
				(next opt:dead/2 (tail Code) Acc true))
			(next opt:dead/2 (tail Code) (cons I Acc)
				(if (opt:is I 'JMP) true
					(if (opt:is I 'LEV) true (opt:is I 'JMPA)))))))
)))

;; ---- the pipeline ----

(define opt:passes (lambda (Code Syms)
	;; adj0 runs again after tail: a same-size tail transform emits ADJ 0
	(opt:dead (opt:adj0 (opt:tail (opt:thread (opt:jmpnext (opt:adj0 (opt:shl (opt:fold Code))))) Syms)))))

(define opt:count-instrs (lambda (Code) (next opt:count/2 Code 0)))
(define opt:count/2 (lambda (Code N)
	(if (empty? Code) N
		(next opt:count/2 (tail Code)
			(if (opt:is-label (head Code)) N (+ N 1))))))

;; Optimize a decoded module to a fixpoint (bounded); returns the new
;; module and prints per-pass statistics.
(define c4opt:optimize (lambda (M) (begin
	(set! opt:n-fold 0) (set! opt:n-shl 0) (set! opt:n-adj0 0)
	(set! opt:n-jmpnext 0) (set! opt:n-thread 0) (set! opt:n-dead 0)
	(set! opt:n-tail 0)
	(define Code (index M 3))
	(define Before (opt:count-instrs Code))
	(define Rounds 0)
	(define Prev -1)
	(define N Before)
	;; iterate until the instruction count stops falling (threading can
	;; enable jmpnext which enables dead, and folds cascade)
	(next c4opt:optloop M Code Before N Prev Rounds))))
(define c4opt:optloop (lambda (M Code Before N Prev Rounds) (begin
	(if (if (= N Prev) true (>= Rounds 10))
		(begin
			(print ";; c4opt:" Before "->" N "instructions in" Rounds "rounds")
			(print ";;   fold" opt:n-fold " mul->shl" opt:n-shl " adj0" opt:n-adj0
				" jmp-next" opt:n-jmpnext " threaded" opt:n-thread " dead" opt:n-dead
				" tail" opt:n-tail)
			(list (head M) (second M) (third M) Code (index M 4)
				(index M 5) (index M 6) (index M 7) (index M 8)))
		(begin
			(set! Code (opt:passes Code (index M 5)))
			(next c4opt:optloop M Code Before (opt:count-instrs Code) N (+ Rounds 1)))))))

)
