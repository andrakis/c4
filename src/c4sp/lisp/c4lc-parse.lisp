;; c4lc-parse.lisp -- C parser for c4lc (docs/c4lc-design.md, L1)
;;
;; (parse:program TOKENS) -> (program TOP...) where TOP is one of
;;   (enum (NAME VAL)...)
;;   (global TYPE "name" SIZE ATTRS INIT)      SIZE nil = scalar, N = array
;;   (proto TYPE "name" (PTYPE...) VARIADIC ATTRS)
;;   (func TYPE "name" ((PTYPE "p")...) VARIADIC ATTRS
;;         (locals (local TYPE "name" SIZE INIT)...) (block STMT...))
;; INIT: nil | (num N) | (str "bytes") | (fnaddr "name") | (braces (N...))
;; stmts: (block S...) (if E S S|nil) (while E S) (for I C P S)
;;        (switch E ITEM...) with (case N)/(default) label items
;;        (break) (continue) (return E|nil) (expr E) (empty)
;; exprs: (num N) (str "s") (var "x") (call "f" A...) (deref E) (addr E)
;;        (lognot E) (bitnot E) (neg E) (preinc E) (predec E)
;;        (postinc E) (postdec E) (cast TYPE E) (sizeof TYPE)
;;        (sizeofa "x") (comma E...) (index E E) (assign L R)
;;        (cond C T F) (add..lor L R)
;;
;; Types are c4cc's integers: CHAR=0, INT=1, +2 per pointer level; void
;; is char. ATTRS is c4cc's bitmask (constructor 1, destructor 2,
;; static 8, extern 0x10, variadic 0x20, array 0x40).
;;
;; M4 (docs/c4lc-design.md, "Integer types"): a band of scalar types
;; at 512 + 64k, k = uchar schar short ushort uint ulong, with the same
;; +2 per pointer level. long is int; unsigned long is ulong.
;;
;; Qualifiers ride BESIDE the type, never in it: a mask with two bits
;; per pointer level (bit 2k const, bit 2k+1 volatile, level 0 the
;; object itself). Where a node carries one it is an extra trailing
;; field, written only when it is not zero, so an unqualified program
;; parses to exactly the tree it always did:
;;   (global TYPE "x" SIZE ATTRS INIT Q)   (local TYPE "x" SIZE INIT Q)
;;   ((PTYPE "p" Q) ...)   (cast TYPE E Q)   member (TYPE "m" ASIZE Q)
;;   (func ... BODY RQ)    (proto ... ATTRS RQ PQS)   (typedefd "T" TYPE Q)
;; and a literal with a U suffix is (num V TYPE).
;;
;; Enum constants are substituted at use sites as (num N), as c4cc's
;; symbol table does; locals/params shadow them. Grammar mirrors
;; c4cc's parse()/stmt()/expr() except: 'for' is parsed CORRECTLY
;; (c4cc's For branch never consumes its token and dies on any use),
;; and checks needing symbol classes (lvalues, sizeof of a non-array
;; identifier) are deferred to sema/codegen.

(begin

;; ---- small helpers ----

(define p:reverse (lambda (l) (next p:reverse/2 l (list))))
(define p:reverse/2 (lambda (l acc)
	(if (empty? l) acc (next p:reverse/2 (tail l) (+ (list (head l)) acc)))))
(define p:cons (lambda (x l) (+ (list x) l)))
(define p:second (lambda (l) (index l 1)))

;; string membership in a list of strings
(define p:member? (lambda (x l)
	(if (empty? l) false
	(if (= (head l) x) true
	(next p:member? x (tail l))))))

;; assoc on 2-lists: (("name" val) ...) -> (name val) or false
(define p:assoc (lambda (k l)
	(if (empty? l) false
	(if (= (head (head l)) k) (head l)
	(next p:assoc k (tail l))))))

;; ---- token stream state ----

(define p:toks nil)
(define p:kind (lambda () (head (head p:toks))))
(define p:value (lambda () (p:second (head p:toks))))
(define p:line (lambda () (index (head p:toks) 2)))
(define p:advance (lambda ()
	(if (tail p:toks) (set! p:toks (tail p:toks)) nil)))

(define p:die (lambda (msg)
	(error (+ "c4lc: line " (p:line) ": " msg))))

(define p:expect (lambda (k what)
	(if (= (p:kind) k) (p:advance) (p:die (+ "expected " what)))))

;; ---- symbol knowledge (enum constants, shadowing locals) ----

(define p:enums nil)      ;; (("NAME" VAL) ...)
(define p:locals nil)     ;; names shadowing enums inside a function

;; enum constant value for the CURRENT Id token, or false
(define p:enumval (lambda ()
	(if (p:member? (p:value) p:locals) false
	(begin
		(define a (p:assoc (p:value) p:enums))
		(if a (p:second a) false)))))

;; ---- types ----

(define p:CHAR 0)
(define p:INT 1)
(define p:PTR 2)
(define p:UCHAR 512)
(define p:SCHAR 576)
(define p:SHORT 640)
(define p:USHORT 704)
(define p:UINT 768)
(define p:ULONG 832)
(define p:WORD (sys:wordsize))
(define p:BITS (* 8 p:WORD))
;; the word's sign bit, and the low 32 bits a 64-bit host keeps a uint
;; in (all ones on a 32-bit host, where uint IS the word)
(define p:MIN (bit:shl 1 (- p:BITS 1)))
(define p:M32 (if (= p:WORD 8) (- (bit:shl 1 32) 1) -1))

;; ---- the type algebra shared with the tree pass ----
;;
;; The arithmetic types after promotion are int, uint and ulong, ranked
;; in that order; everything narrower promotes to int. "Word-unsigned"
;; is ulong, and uint on a 32-bit host where the two are the same word.
(define p:promote (lambda (t)
	(if (= t p:UINT) p:UINT (if (= t p:ULONG) p:ULONG p:INT))))
(define p:uac (lambda (a b)
	(begin
		(define pa (p:promote a))
		(define pb (p:promote b))
		(if (if (= pa p:ULONG) true (= pb p:ULONG)) p:ULONG
		(if (if (= pa p:UINT) true (= pb p:UINT)) p:UINT
		p:INT)))))

;; a value converted to an integer type, the way the generated code
;; converts it
(define p:sext (lambda (v n)
	(bit:shr (bit:shl v (- p:BITS n)) (- p:BITS n))))
(define p:convval (lambda (v t)
	(if (if (= t p:CHAR) true (= t p:SCHAR)) (p:sext v 8)
	(if (= t p:UCHAR) (bit:and v 255)
	(if (= t p:SHORT) (p:sext v 16)
	(if (= t p:USHORT) (bit:and v 65535)
	(if (= t p:UINT) (bit:and v p:M32)
	v)))))))

;; unsigned order and division on a whole word
(define p:ult (lambda (a b) (< (bit:xor a p:MIN) (bit:xor b p:MIN))))
(define p:udiv (lambda (a b)
	(if (< b 0) (if (p:ult a b) 0 1)
	(if (>= a 0) (/ a b)
	(begin
		(define q (bit:shl (/ (bit:and (bit:shr a 1) (bit:xor p:MIN -1)) b) 1))
		(if (p:ult (- a (* q b)) b) q (+ q 1)))))))
(define p:umod (lambda (a b) (- a (* (p:udiv a b) b))))

;; c4's arithmetic on two typed constants. Returns the value and leaves
;; its type in p:ft; 'nofold for an operator it does not know. The
;; signed branch is exactly what this file and the tree pass always
;; did (truncating % and an ARITHMETIC >>), so a program with no
;; unsigned in it folds to the same numbers it always has.
(define p:ft 1)
(define p:tfold (lambda (op a at b bt)
	(begin
		(define shift (if (= op 'shl) true (= op 'shr)))
		(define r (if shift (p:promote at) (p:uac at bt)))
		(define cmp (if (= op 'eq) true (if (= op 'ne) true (if (= op 'lt) true
			(if (= op 'gt) true (if (= op 'le) true (= op 'ge)))))))
		(set! p:ft (if cmp p:INT r))
		(if (= r p:INT) (p:sarith op a b)
		(if (if (= r p:UINT) (= p:WORD 8) false)
			(p:narith op (bit:and a p:M32) (if shift b (bit:and b p:M32)))
		(p:uarith op a b))))))
(define p:sarith (lambda (op a b)
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
;; uint on a 64-bit host: both operands are in [0, 2^32), where the
;; signed operations already give the unsigned answer; only what can
;; leave that range is cut back into it
(define p:narith (lambda (op a b)
	(begin
		(define v (p:sarith op a b))
		(if (= v 'nofold) v
		(if (if (= op 'add) true (if (= op 'sub) true (if (= op 'mul) true
			(if (= op 'shl) true (if (= op 'bor) true (= op 'bxor))))))
			(bit:and v p:M32)
		v)))))
;; a whole unsigned word
(define p:uarith (lambda (op a b)
	(if (= op 'div) (p:udiv a b)
	(if (= op 'mod) (p:umod a b)
	(if (= op 'shr)
		(if (= b 0) a (bit:and (bit:shr a b) (bit:xor (bit:shl -1 (- p:BITS b)) -1)))
	(if (= op 'lt) (if (p:ult a b) 1 0)
	(if (= op 'gt) (if (p:ult b a) 1 0)
	(if (= op 'le) (if (p:ult b a) 0 1)
	(if (= op 'ge) (if (p:ult a b) 0 1)
	(p:sarith op a b))))))))))

;; the qualifier mask of a declarator after its stars, and of a base type
;; after its specifiers (mailboxes, as lex:val is)
(define p:bq 0)
(define p:sq 0)
;; two bits per level; the const bits of every level below the object's
;; own, which is what dropping a const is about
(define p:qnode (lambda (node q) (if (= q 0) node (+ node (list q)))))

;; struct types are encoded as 1024 + 64*id + 2*ptrlevel: 64 per
;; struct leaves 31 pointer levels, and ty >= 1024 tests structness.
;; The parser owns the registries; layout happens in gen.
(define p:STRUCT0 1024)
(define p:SSTEP 64)
(define p:structs nil)     ;; (("Name" TYPEID) ...)
(define p:nstructs 0)
(define p:typedefs nil)    ;; (("Name" TYPE) ...)
(define p:pending nil)     ;; structdef nodes awaiting emission

;; type id for a struct name, registering a forward reference if new
(define p:structid (lambda (n)
	(begin
		(define a (p:assoc n p:structs))
		(if a (p:second a)
		(begin
			(define ty (+ p:STRUCT0 (* p:SSTEP p:nstructs)))
			(set! p:nstructs (+ p:nstructs 1))
			(set! p:structs (p:cons (list n ty) p:structs))
			ty)))))

;; struct S / union U [{ members }] -- returns the value type; a body
;; queues a (structdef NAME ISUNION MEMBERS) node on p:pending
(define p:structtype (lambda ()
	(begin
		(define isu (= (p:kind) 'Union))
		(p:advance)
		(define n
			(if (= (p:kind) 'Id)
				(begin (define v (p:value)) (p:advance) v)
			(+ "@anon" p:nstructs)))
		(define ty (p:structid n))
		(if (= (p:kind) 'Lbrace)
			(begin
				(p:advance)
				(set! p:pending (p:cons
					(list 'structdef n ty isu (p:members (list)))
					p:pending)))
			nil)
		ty)))
(define p:members (lambda (acc)
	(if (= (p:kind) 'Rbrace)
		(begin (p:advance) (p:reverse acc))
	(begin
		(define bt (p:basetype nil))
		(if (= bt nil) (p:die "bad struct member type") nil)
		(next p:members/2 bt p:bq acc)))))
(define p:members/2 (lambda (bt bq acc)
	(begin
		(define ty (p:stars bt bq))
		(define q p:sq)
		(if (= (p:kind) 'Id) nil (p:die "bad struct member name"))
		(define n (p:value))
		(p:advance)
		(define size (p:arraysize))
		(define acc2 (p:cons
			(p:qnode (list (if (= size nil) ty (+ ty p:PTR)) n size)
				(if (= size nil) q (bit:shl q 2)))
			acc))
		(if (= (p:kind) 'Comma)
			(begin (p:advance) (next p:members/2 bt bq acc2))
		(begin
			(p:expect 'Semi "semicolon after struct member")
			(next p:members acc2))))))

;; optional base type: any run of the specifiers C allows -- const,
;; volatile, signed, unsigned, short, long, int, char, a struct/union, or
;; a typedef name (which only counts while no other type word has been
;; seen, or `unsigned T;` could never declare a T). Nothing at all ->
;; default d (nil marks "no type seen" for callers that must know). The
;; qualifiers land in p:bq.
(define p:basetype (lambda (d) (p:spec d 0 0 0 0 0 0 0 nil)))
(define p:spec (lambda (d q sg un sh lg ch it bt)
	(begin
		(define k (p:kind))
		(define bare (if (= bt nil)
			(= 0 (+ sg (+ un (+ sh (+ lg (+ ch it)))))) false))
		(if (= k 'Const) (begin (p:advance) (next p:spec d (bit:or q 1) sg un sh lg ch it bt))
		(if (= k 'Volatile) (begin (p:advance) (next p:spec d (bit:or q 2) sg un sh lg ch it bt))
		(if (= k 'Signed) (begin (p:advance) (next p:spec d q 1 un sh lg ch it bt))
		(if (= k 'Unsigned) (begin (p:advance) (next p:spec d q sg 1 sh lg ch it bt))
		(if (= k 'Short) (begin (p:advance) (next p:spec d q sg un 1 lg ch it bt))
		(if (= k 'Long) (begin (p:advance) (next p:spec d q sg un sh (+ lg 1) ch it bt))
		(if (= k 'Char) (begin (p:advance) (next p:spec d q sg un sh lg 1 it bt))
		(if (= k 'Int) (begin (p:advance) (next p:spec d q sg un sh lg ch 1 bt))
		(if (if bare (if (= k 'Struct) true (= k 'Union)) false)
			(begin
				(define st (p:structtype))
				(next p:spec d q sg un sh lg ch it st))
		(if (if bare (= k 'Id) false)
			(begin
				(define a (p:assoc (p:value) p:typedefs))
				(if a
					(begin
						(p:advance)
						(next p:spec d (bit:or q (index a 2)) sg un sh lg ch it (p:second a)))
				(p:specdone d q sg un sh lg ch it bt)))
		(p:specdone d q sg un sh lg ch it bt))))))))))))))
(define p:specdone (lambda (d q sg un sh lg ch it bt)
	(begin
		(set! p:bq q)
		(if (= bt nil)
			(if (= ch 1) (if (= un 1) p:UCHAR (if (= sg 1) p:SCHAR p:CHAR))
			(if (= sh 1) (if (= un 1) p:USHORT p:SHORT)
			(if (> lg 0) (if (= un 1) p:ULONG p:INT)
			(if (= un 1) p:UINT
			(if (= sg 1) p:INT
			(if (= it 1) p:INT
			d))))))
		bt))))

;; is the current token the start of a declaration?
(define p:declstart? (lambda ()
	(begin
		(define k (p:kind))
		(if (= k 'Int) true
		(if (= k 'Char) true
		(if (= k 'Struct) true
		(if (= k 'Union) true
		(if (= k 'Const) true
		(if (= k 'Volatile) true
		(if (= k 'Signed) true
		(if (= k 'Unsigned) true
		(if (= k 'Short) true
		(if (= k 'Long) true
		(if (= k 'Id)
			(if (p:assoc (p:value) p:typedefs) true false)
		false))))))))))))))

;; consume the declarator's stars, each followed by its own qualifiers:
;; ty -> ty + 2 per star, and the mask q of the base moves up a level
;; per star. The declarator's mask is left in p:sq.
(define p:stars (lambda (ty q)
	(if (= (p:kind) 'Mul) (begin (p:advance) (next p:stars (+ ty p:PTR) (bit:shl q 2)))
	(if (= (p:kind) 'Const) (begin (p:advance) (next p:stars ty (bit:or q 1)))
	(if (= (p:kind) 'Volatile) (begin (p:advance) (next p:stars ty (bit:or q 2)))
	(begin (set! p:sq q) ty))))))

;; ---- constants: an integer constant expression ----

;; Literals, character literals, enum names, and the arithmetic between
;; them. Array sizes, case labels, initializers and enum values are the
;; places C requires one, and "int t[MAX * WORDS]" is ordinary enough
;; that refusing it is a bug rather than a simplification.
;;
;; The operator set is C's integer-constant subset, in C's precedence
;; order: | ^ & << >> + - * / % and the unary - + ~ !. The unary case
;; recurses, so "- -1" and "!!X" parse; the old hand-rolled single-flag
;; negation could not. ~x is written -x-1 rather than a bit:not, which
;; c4sp does not have, and % uses the truncating form from p:sarith
;; (c4lc-tree.lisp) so the folded value matches what the VM computes.
;;
;; M4: every value carries its type (in the p:ct mailbox), and two are
;; combined by p:tfold -- the same function the tree pass folds with --
;; so `0xFFFFFFFFu >> 4` is a logical shift and `-1 < 1u` is false here
;; exactly as it is at run time.
(define p:ct 1)
(define p:sconsts nil)    ;; (("NAME" VAL TYPE) ...): static const scalars

;; a literal's value and type, from its token
(define p:numtok (lambda ()
	(begin
		(define k (p:kind))
		(define v (p:value))
		(p:advance)
		(set! p:ct
			(if (= k 'NumUL) p:ULONG
			(if (= k 'NumU)
				(if (= p:WORD 8) (if (if (< v 0) true (> v p:M32)) p:ULONG p:UINT) p:UINT)
			p:INT)))
		v)))
(define p:numtok? (lambda (k)
	(if (= k 'Num) true (if (= k 'NumU) true (= k 'NumUL)))))

;; a static const scalar named by the current token, or false
(define p:sconst (lambda ()
	(if (p:member? (p:value) p:locals) false
		(p:assoc (p:value) p:sconsts))))

(define p:constatom (lambda (what)
	(if (= (p:kind) 'Sub)
		(begin (p:advance)
			(define v (p:constatom what))
			(define r (p:tfold 'sub 0 p:INT v p:ct))
			(set! p:ct p:ft) r)
	(if (= (p:kind) 'Add)   (begin (p:advance) (p:constatom what))
	(if (= (p:kind) 'Tilde)
		(begin (p:advance)
			(define v (p:constatom what))
			(define r (p:tfold 'bxor -1 p:INT v p:ct))
			(set! p:ct p:ft) r)
	(if (= (p:kind) 'Not)
		(begin (p:advance)
			(define v (p:constatom what))
			(set! p:ct p:INT)
			(if (= 0 v) 1 0))
	(if (p:numtok? (p:kind)) (p:numtok)
	(if (= (p:kind) 'Lparen)
		(begin
			(p:advance)
			(define inner (p:const what))
			(define it p:ct)
			(p:expect 'Rparen ") in constant expression")
			(set! p:ct it)
			inner)
	(if (= (p:kind) 'Id)
		(begin
			(define ev (p:enumval))
			(define sc (if (= ev false) (p:sconst) false))
			(if (= ev false)
				(if (= sc false) (p:die (+ what " must be an integer constant"))
					(begin (p:advance) (set! p:ct (index sc 2)) (p:second sc)))
				(begin (p:advance) (set! p:ct p:INT) ev)))
	(p:die (+ what " must be an integer constant")))))))))))

;; one binary step: l (of type lt) op the next operand
(define p:cstep (lambda (op l lt r)
	(begin
		(define v (p:tfold op l lt r p:ct))
		(set! p:ct p:ft)
		v)))

(define p:constmul (lambda (what)
	(begin (define v (p:constatom what)) (next p:constmul/2 what v p:ct))))
(define p:constmul/2 (lambda (what l lt)
	(if (= (p:kind) 'Mul)
		(begin (p:advance) (define r (p:constatom what))
			(define v (p:cstep 'mul l lt r)) (next p:constmul/2 what v p:ct))
	(if (= (p:kind) 'Div)
		(begin (p:advance) (define r (p:constatom what))
			(define v (p:cstep 'div l lt r)) (next p:constmul/2 what v p:ct))
	(if (= (p:kind) 'Mod)
		(begin (p:advance) (define r (p:constatom what))
			(define v (p:cstep 'mod l lt r)) (next p:constmul/2 what v p:ct))
	(begin (set! p:ct lt) l))))))

(define p:constadd (lambda (what)
	(begin (define v (p:constmul what)) (next p:constadd/2 what v p:ct))))
(define p:constadd/2 (lambda (what l lt)
	(if (= (p:kind) 'Add)
		(begin (p:advance) (define r (p:constmul what))
			(define v (p:cstep 'add l lt r)) (next p:constadd/2 what v p:ct))
	(if (= (p:kind) 'Sub)
		(begin (p:advance) (define r (p:constmul what))
			(define v (p:cstep 'sub l lt r)) (next p:constadd/2 what v p:ct))
	(begin (set! p:ct lt) l)))))

(define p:constshift (lambda (what)
	(begin (define v (p:constadd what)) (next p:constshift/2 what v p:ct))))
(define p:constshift/2 (lambda (what l lt)
	(if (= (p:kind) 'Shl)
		(begin (p:advance) (define r (p:constadd what))
			(define v (p:cstep 'shl l lt r)) (next p:constshift/2 what v p:ct))
	(if (= (p:kind) 'Shr)
		(begin (p:advance) (define r (p:constadd what))
			(define v (p:cstep 'shr l lt r)) (next p:constshift/2 what v p:ct))
	(begin (set! p:ct lt) l)))))

(define p:constand (lambda (what)
	(begin (define v (p:constshift what)) (next p:constand/2 what v p:ct))))
(define p:constand/2 (lambda (what l lt)
	(if (= (p:kind) 'And)
		(begin (p:advance) (define r (p:constshift what))
			(define v (p:cstep 'band l lt r)) (next p:constand/2 what v p:ct))
	(begin (set! p:ct lt) l))))

(define p:constxor (lambda (what)
	(begin (define v (p:constand what)) (next p:constxor/2 what v p:ct))))
(define p:constxor/2 (lambda (what l lt)
	(if (= (p:kind) 'Xor)
		(begin (p:advance) (define r (p:constand what))
			(define v (p:cstep 'bxor l lt r)) (next p:constxor/2 what v p:ct))
	(begin (set! p:ct lt) l))))

(define p:const (lambda (what)
	(begin (define v (p:constxor what)) (next p:const/2 what v p:ct))))
(define p:const/2 (lambda (what l lt)
	(if (= (p:kind) 'Or)
		(begin (p:advance) (define r (p:constxor what))
			(define v (p:cstep 'bor l lt r)) (next p:const/2 what v p:ct))
	(begin (set! p:ct lt) l))))

;; ---- expressions ----

;; operator precedence levels, mirroring c4cc's token enum order;
;; 0 = not a binary/postfix operator
(define p:levels '(
	(Assign 1) (Cond 2) (Lor 3) (Lan 4) (Or 5) (Xor 6) (And 7)
	(Eq 8) (Ne 9) (Lt 10) (Gt 11) (Le 12) (Ge 13) (Shl 14) (Shr 15)
	(Add 16) (Sub 17) (Mul 18) (Div 19) (Mod 20)
	(Inc 21) (Dec 22) (Brak 23) (Dot 24) (Arrow 24)
	(AddA 1) (SubA 1) (MulA 1) (DivA 1) (ModA 1)
	(AndA 1) (OrA 1) (XorA 1) (ShlA 1) (ShrA 1)))

;; compound assignment: desugared to L = L op R (the lvalue is parsed
;; once but EMITTED twice; a[i++] += x is out of contract)
(define p:compound '(
	(AddA add) (SubA sub) (MulA mul) (DivA div) (ModA mod)
	(AndA band) (OrA bor) (XorA bxor) (ShlA shl) (ShrA shr)))
(define p:level (lambda (k)
	(begin
		(define a (p:assoc/atom k p:levels))
		(if a (p:second a) 0))))
(define p:assoc/atom (lambda (k l)   ;; assoc with atom keys
	(if (empty? l) false
	(if (= (head (head l)) k) (head l)
	(next p:assoc/atom k (tail l))))))

;; binary operator AST heads, and the rhs precedence for each
(define p:binops '(
	(Lor lor 4) (Lan land 5) (Or bor 6) (Xor bxor 7) (And band 8)
	(Eq eq 10) (Ne ne 10) (Lt lt 14) (Gt gt 14) (Le le 14) (Ge ge 14)
	(Shl shl 16) (Shr shr 16) (Add add 18) (Sub sub 18)
	(Mul mul 21) (Div div 21) (Mod mod 21)))

;; string literal with adjacent-literal concatenation (expressions only)
(define p:strlit (lambda ()
	(begin
		(define s (p:value))
		(p:advance)
		(next p:strlit/2 s))))
(define p:strlit/2 (lambda (s)
	(if (= (p:kind) 'Str)
		(begin
			(define t (p:value))
			(p:advance)
			(next p:strlit/2 (+ s t)))
	(list 'str s))))

;; call argument list, after the open paren is consumed
(define p:args (lambda (acc)
	(if (= (p:kind) 'Rparen)
		(begin (p:advance) (p:reverse acc))
	(begin
		(define a (p:expr 1))
		(if (= (p:kind) 'Comma) (p:advance) nil)
		(next p:args (p:cons a acc))))))

;; sizeof(...): type (including struct/union/typedef names) or an
;; array identifier
(define p:sizeof (lambda ()
	(begin
		(p:advance)
		(p:expect 'Lparen "( after sizeof")
		(define r
			(if (if (= (p:kind) 'Id)
					(= (p:assoc (p:value) p:typedefs) false)
					false)
				(begin
					(define n (p:value))
					(p:advance)
					(list 'sizeofa n))
			(begin
				(define bt (p:basetype p:INT))
				(define ty (p:stars bt p:bq))
				(list 'sizeof ty))))
		(p:expect 'Rparen ") after sizeof")
		r)))

;; parenthesized: cast, or (expr[, expr...]). A cast begins with a
;; type keyword or a typedef name -- the registries disambiguate
;; (T)x from a parenthesized variable.
(define p:parenexpr (lambda ()
	(begin
		(p:advance)
		(if (p:declstart?)
			(begin
				(define bt (p:basetype p:INT))
				(define ty (p:stars bt p:bq))
				(define cq p:sq)
				(p:expect 'Rparen "closing paren of cast")
				(p:qnode (list 'cast ty (p:expr 21)) cq))
		(begin
			(define e1 (p:expr 1))
			(define es (p:commalist (list e1)))
			(p:expect 'Rparen "close paren")
			(if (empty? (tail es)) (head es) (p:cons 'comma es)))))))
(define p:commalist (lambda (acc)
	(if (= (p:kind) 'Comma)
		(begin
			(p:advance)
			(next p:commalist (p:cons (p:expr 1) acc)))
	(p:reverse acc))))

;; primary / unary
(define p:primary (lambda ()
	(begin
		(define k (p:kind))
		(if (= k 'Num) (begin (define n (p:value)) (p:advance) (list 'num n))
		(if (p:numtok? k)
			(begin
				(define un (p:numtok))
				(list 'num (if (= p:ct p:UINT) (bit:and un p:M32) un) p:ct))
		(if (= k 'Str) (p:strlit)
		(if (= k 'Sizeof) (p:sizeof)
		(if (= k 'Id) (p:idexpr)
		(if (= k 'Lparen) (p:parenexpr)
		(if (= k 'Mul) (begin (p:advance) (list 'deref (p:expr 21)))
		(if (= k 'And) (begin (p:advance) (list 'addr (p:expr 21)))
		(if (= k 'Not) (begin (p:advance) (list 'lognot (p:expr 21)))
		(if (= k 'Tilde) (begin (p:advance) (list 'bitnot (p:expr 21)))
		(if (= k 'Add) (begin (p:advance) (p:expr 21))
		(if (= k 'Sub) (p:negexpr)
		(if (= k 'Inc) (begin (p:advance) (list 'preinc (p:expr 21)))
		(if (= k 'Dec) (begin (p:advance) (list 'predec (p:expr 21)))
		(p:die "bad expression"))))))))))))))))))

;; unary minus: fold a literal, as c4cc does
(define p:negexpr (lambda ()
	(begin
		(p:advance)
		(if (= (p:kind) 'Num)
			(begin
				(define n (p:value))
				(p:advance)
				(list 'num (- 0 n)))
		(list 'neg (p:expr 21))))))

;; identifier: call, enum constant, or variable
(define p:idexpr (lambda ()
	(begin
		(define n (p:value))
		(define ev (p:enumval))
		(p:advance)
		(if (= (p:kind) 'Lparen)
			(begin
				(p:advance)
				(p:cons 'call (p:cons n (p:args (list)))))
		(if (= ev false) (list 'var n)
		(list 'num ev))))))

;; precedence climbing over a parsed left node
(define p:expr (lambda (lev)
	(p:climb (p:primary) lev)))

(define p:climb (lambda (lhs lev)
	(begin
		(define k (p:kind))
		(define l (p:level k))
		(if (< l lev) lhs
		(if (= k 'Assign)
			(begin
				(p:advance)
				(next p:climb (list 'assign lhs (p:expr 1)) lev))
		(if (= k 'Cond)
			(begin
				(p:advance)
				(define t (p:expr 1))
				(p:expect 'Colon "colon in conditional")
				(next p:climb (list 'cond lhs t (p:expr 2)) lev))
		(if (= k 'Inc)
			(begin (p:advance) (next p:climb (list 'postinc lhs) lev))
		(if (= k 'Dec)
			(begin (p:advance) (next p:climb (list 'postdec lhs) lev))
		(if (= k 'Brak)
			(begin
				(p:advance)
				(define i (p:expr 1))
				(p:expect 'Rbrak "close bracket")
				(next p:climb (list 'index lhs i) lev))
		(if (= k 'Dot)
			(begin
				(p:advance)
				(if (= (p:kind) 'Id) nil (p:die "member name expected after ."))
				(define mn (p:value))
				(p:advance)
				(next p:climb (list 'member lhs mn) lev))
		(if (= k 'Arrow)
			(begin
				(p:advance)
				(if (= (p:kind) 'Id) nil (p:die "member name expected after ->"))
				(define an (p:value))
				(p:advance)
				(next p:climb (list 'arrow lhs an) lev))
		(begin
			(define ca (p:assoc/atom k p:compound))
			(if ca
				(begin
					(p:advance)
					(next p:climb
						(list 'assign lhs
							(list (p:second ca) lhs (p:expr 1)))
						lev))
			(begin
				(define b (p:assoc/atom k p:binops))
				(if (= b false) (p:die "bad operator")
				(begin
					(p:advance)
					(next p:climb
						(list (p:second b) lhs (p:expr (index b 2)))
						lev)))))))))))))))))

;; ---- statements ----

(define p:stmt (lambda ()
	(begin
		(define k (p:kind))
		(if (= k 'If) (p:ifstmt)
		(if (= k 'While) (p:whilestmt)
		(if (= k 'For) (p:forstmt)
		(if (= k 'Switch) (p:switchstmt)
		(if (= k 'Break)
			(begin (p:advance) (p:expect 'Semi "semicolon after break") '(break))
		(if (= k 'Continue)
			(begin (p:advance) (p:expect 'Semi "semicolon after continue") '(continue))
		(if (= k 'Return) (p:returnstmt)
		(if (= k 'Lbrace) (p:block)
		(if (= k 'Semi) (begin (p:advance) '(empty))
		(if (= k 'Do) (p:dostmt)
		(if (p:declstart?)
			(p:cons 'declstmt (p:localline))
		(begin
			(define e (p:expr 1))
			(p:expect 'Semi "semicolon")
			(list 'expr e))))))))))))))))

(define p:dostmt (lambda ()
	(begin
		(p:advance)
		(define s (p:stmt))
		(p:expect 'While "while after do body")
		(p:expect 'Lparen "open paren after do-while")
		(define c (p:expr 1))
		(p:expect 'Rparen "close paren after do-while condition")
		(p:expect 'Semi "semicolon after do-while")
		(list 'dowhile s c))))

(define p:block (lambda ()
	(begin
		(p:advance)
		(next p:block/2 (list)))))
(define p:block/2 (lambda (acc)
	(if (= (p:kind) 'Rbrace)
		(begin (p:advance) (p:cons 'block (p:reverse acc)))
	(next p:block/2 (p:cons (p:stmt) acc)))))

(define p:ifstmt (lambda ()
	(begin
		(p:advance)
		(p:expect 'Lparen "open paren after if")
		(define c (p:expr 1))
		(p:expect 'Rparen "close paren after if condition")
		(define t (p:stmt))
		(if (= (p:kind) 'Else)
			(begin
				(p:advance)
				(list 'if c t (p:stmt)))
		(list 'if c t nil)))))

(define p:whilestmt (lambda ()
	(begin
		(p:advance)
		(p:expect 'Lparen "open paren after while")
		(define c (p:expr 1))
		(p:expect 'Rparen "close paren after while condition")
		(list 'while c (p:stmt)))))

;; for (init; cond; step) stmt -- parsed correctly (see header); each
;; part may be empty
(define p:forstmt (lambda ()
	(begin
		(p:advance)
		(p:expect 'Lparen "open paren after for")
		(define i (if (= (p:kind) 'Semi) nil (p:expr 1)))
		(p:expect 'Semi "semicolon after for initializer")
		(define c (if (= (p:kind) 'Semi) nil (p:expr 1)))
		(p:expect 'Semi "semicolon after for condition")
		(define s (if (= (p:kind) 'Rparen) nil (p:expr 1)))
		(p:expect 'Rparen "close paren after for")
		(list 'for i c s (p:stmt)))))

(define p:returnstmt (lambda ()
	(begin
		(p:advance)
		(if (= (p:kind) 'Semi)
			(begin (p:advance) (list 'return nil))
		(begin
			(define e (p:expr 1))
			(p:expect 'Semi "semicolon after return")
			(list 'return e))))))

;; switch (expr) { case C: ... default: ... }: case/default only at
;; the top level of the switch braces, exactly as c4cc
(define p:switchstmt (lambda ()
	(begin
		(p:advance)
		(p:expect 'Lparen "open paren after switch")
		(define c (p:expr 1))
		(p:expect 'Rparen "close paren after switch value")
		(p:expect 'Lbrace "open brace after switch")
		(p:cons 'switch (p:cons c (p:switchbody (list)))))))
(define p:switchbody (lambda (acc)
	(begin
		(define k (p:kind))
		(if (= k 'Rbrace)
			(begin (p:advance) (p:reverse acc))
		(if (= k 'Case)
			(begin
				(p:advance)
				(define v (p:const "'case'"))
				(p:expect 'Colon "colon after case")
				(next p:switchbody (p:cons (list 'case v) acc)))
		(if (= k 'Default)
			(begin
				(p:advance)
				(p:expect 'Colon "colon after default")
				(next p:switchbody (p:cons '(default) acc)))
		(next p:switchbody (p:cons (p:stmt) acc))))))))

;; ---- declarations ----

;; __attribute__((constructor|destructor)) between stars and the name;
;; returns the attr bit
(define p:attribute (lambda ()
	(begin
		(p:advance)
		(p:expect 'Lparen "( after __attribute__")
		(p:expect 'Lparen "(( after __attribute__")
		(define a
			(if (= (p:kind) 'Constructor) 1
			(if (= (p:kind) 'Destructor) 2
			(p:die "unknown attribute"))))
		(p:advance)
		(p:expect 'Rparen ") closing attribute")
		(p:expect 'Rparen ")) closing attribute")
		a)))

;; optional [size]: returns nil (no array), N (elements), or 0 (from
;; the initializer)
(define p:arraysize (lambda ()
	(if (= (p:kind) 'Brak)
		(begin
			(p:advance)
			(define s
				(if (= (p:kind) 'Rbrak) 0
				(p:const "array size")))
			(p:expect 'Rbrak "] after array size")
			s)
	nil)))

;; initializer after '=': (num N) | (str S) | (fnaddr F) | (braces (N...))
(define p:init (lambda ()
	(begin
		(define k (p:kind))
		(if (= k 'Lbrace) (p:braceinit)
		(if (= k 'Str)
			(begin
				(define s (p:value))
				(p:advance)
				(list 'str s))
		(if (= k 'And)
			(begin
				(p:advance)
				(if (= (p:kind) 'Id)
					(begin
						(define n (p:value))
						(p:advance)
						(list 'fnaddr n))
				(p:die "& initializer requires a function name")))
		(list 'num (p:const "initializer"))))))))
(define p:braceinit (lambda ()
	(begin
		(p:advance)
		(next p:braceinit/2 (list)))))
(define p:braceinit/2 (lambda (acc)
	(if (= (p:kind) 'Rbrace)
		(begin (p:advance) (list 'braces (p:reverse acc)))
	(begin
		(define v (p:const "array initializer"))
		(if (= (p:kind) 'Comma) (p:advance) nil)
		(next p:braceinit/2 (p:cons v acc))))))

;; resolve an array's element count from its initializer when [] was
;; given; also the implicit size of char t[] = "...". Returns the
;; final SIZE for the AST node (nil = scalar).
(define p:initsize (lambda (size elem init what)
	(if (= size nil) nil
	(if (> size 0) size
	(if (= init nil) (p:die (+ what ": array [] needs an initializer"))
	(if (= (head init) 'braces) (length (p:second init))
	(if (if (= (head init) 'str) (p:bytety? elem) false)
		(+ (length (p:second init)) 1)
	(p:die (+ what ": cannot size array from this initializer")))))))))

;; ---- enum declaration ----

(define p:enumdecl (lambda ()
	(begin
		(p:advance)
		(if (= (p:kind) 'Lbrace) nil (p:advance))  ;; skip optional tag
		(if (= (p:kind) 'Lbrace)
			(begin
				(p:advance)
				(p:cons 'enum (p:enumbody 0 (list))))
		(list 'enum)))))
(define p:enumbody (lambda (i acc)
	(if (= (p:kind) 'Rbrace)
		(begin (p:advance) (p:reverse acc))
	(begin
		(if (= (p:kind) 'Id) nil (p:die "bad enum identifier"))
		(define n (p:value))
		(p:advance)
		;; Any integer constant expression, not just [-]Num. p:enums is
		;; updated below BEFORE the recursive call, so a constant named
		;; earlier in this same body is already visible here:
		;; enum { A = 1, B = A + 1 } works.
		(define v
			(if (= (p:kind) 'Assign)
				(begin (p:advance) (p:const "enum initializer"))
			i))
		(if (= (p:kind) 'Comma) (p:advance) nil)
		(set! p:enums (p:cons (list n v) p:enums))
		(next p:enumbody (+ v 1) (p:cons (list n v) acc))))))

;; ---- function parameters ----

;; returns (PARAMS VARIADIC): PARAMS = ((TYPE "name") ...)
(define p:params (lambda (acc)
	(begin
		(define k (p:kind))
		(if (= k 'Rparen)
			(begin (p:advance) (list (p:reverse acc) false))
		(if (= k 'Dot)
			(begin
				(p:advance)
				(p:expect 'Dot "expected more dots")
				(p:expect 'Dot "expected more dots")
				(p:expect 'Rparen ") after ...")
				(list (p:reverse acc) true))
		(begin
			(if (= k 'Static) (p:die "parameters cannot be marked static") nil)
			(define bt (p:basetype p:INT))
			(define ty (p:stars bt p:bq))
			(define q p:sq)
			(if (p:svalue? ty)
				(p:die "struct parameters must be pointers") nil)
			(if (= (p:kind) 'Id) nil (p:die "bad parameter declaration"))
			(define n (p:value))
			(p:advance)
			(if (= (p:kind) 'Comma) (p:advance) nil)
			(next p:params (p:cons (p:qnode (list ty n) q) acc))))))))

;; ---- local declarations ----

;; one "int|char decl, decl;" line; returns local nodes (order kept)
(define p:localline (lambda ()
	(begin
		(if (= (p:kind) 'Static) (p:die "static function variables not implemented") nil)
		(define bt (p:basetype p:INT))
		(next p:localline/2 bt p:bq (list)))))
(define p:localline/2 (lambda (bt bq acc)
	(if (= (p:kind) 'Semi)
		(begin (p:advance) (p:reverse acc))
	(begin
		(define ty (p:stars bt bq))
		(define q p:sq)
		(if (= (p:kind) 'Id) nil (p:die "bad local declaration"))
		(define n (p:value))
		(p:advance)
		(define elem ty)
		(define size (p:arraysize))
		(define ty2 (if (= size nil) ty (+ ty p:PTR)))
		(define q2 (if (= size nil) q (bit:shl q 2)))
		;; scalars take a full (possibly non-constant) initializer
		;; expression; arrays keep the constant forms
		(define init
			(if (= (p:kind) 'Assign)
				(begin
					(p:advance)
					(if (= size nil)
						(list 'einit (p:expr 1))
					(p:init)))
			nil))
		(define fsize (p:initsize size elem init "local"))
		(set! p:locals (p:cons n p:locals))
		(if (= (p:kind) 'Comma) (p:advance) nil)
		(next p:localline/2 bt bq
			(p:cons (p:qnode (list 'local ty2 n fsize init) q2) acc))))))

;; all locals at the top of a function body (declarations can also
;; appear anywhere in a block; those become declstmt nodes)
(define p:localdecls (lambda (acc)
	(if (if (p:declstart?) true (= (p:kind) 'Static))
		(next p:localdecls (+ acc (p:localline)))
	(p:cons 'locals acc))))

;; struct VALUE type: ty >= 1024 with pointer level 0
(define p:svalue? (lambda (ty)
	(if (< ty p:STRUCT0) false
	(= 0 (- (- ty p:STRUCT0)
		(* p:SSTEP (/ (- ty p:STRUCT0) p:SSTEP)))))))

;; the one-byte element types: an array of them packs, and a string can
;; initialize one
(define p:bytety? (lambda (t)
	(if (= t p:CHAR) true (if (= t p:UCHAR) true (= t p:SCHAR)))))

;; ---- top-level declarator list ----

;; one declarator, base type bt (qualifiers bq), prefix attrs; returns a
;; TOP node
(define p:declarator (lambda (bt bq attr)
	(begin
		(define ty (p:stars bt bq))
		(define q p:sq)
		(define a2
			(if (= (p:kind) 'Attribute) (+ attr (p:attribute)) attr))
		(if (= (p:kind) 'Id) nil (p:die "bad global declaration"))
		(define n (p:value))
		(p:advance)
		(if (= (p:kind) 'Lparen)
			(p:function ty n a2 q)
		(p:globalvar ty n a2 q)))))

(define p:function (lambda (ty n attr rq)
	(begin
		(p:advance)
		(define pv (p:params (list)))
		(define ps (head pv))
		(define va (p:second pv))
		(define a2 (if va (+ attr 32) attr))
		(if (= (p:kind) 'Semi)
			;; prototype: extern, parameter types only; the ';' stays
			;; for the declarator loop (as in c4cc), so "int f(), g;"
			;; keeps working. The masks, when there are any, follow.
			(begin
				(define pq (p:pquals ps (list)))
				(define pn (list 'proto ty n (p:ptypes ps (list)) va (+ a2 16)))
				(if (if (= rq 0) (p:allzero? pq) false) pn
					(+ pn (list rq pq))))
		(begin
			(p:expect 'Lbrace "function body")
			(set! p:locals (p:paramnames ps (list)))
			(define ls (p:localdecls (list)))
			(define body (p:block/2 (list)))
			(set! p:locals (list))
			(p:qnode (list 'func ty n ps va a2 ls body) rq))))))
(define p:ptypes (lambda (ps acc)
	(if (empty? ps) (p:reverse acc)
	(next p:ptypes (tail ps) (p:cons (head (head ps)) acc)))))
(define p:pquals (lambda (ps acc)
	(if (empty? ps) (p:reverse acc)
	(next p:pquals (tail ps)
		(p:cons (if (= (index (head ps) 2) nil) 0 (index (head ps) 2)) acc)))))
(define p:allzero? (lambda (l)
	(if (empty? l) true (if (= (head l) 0) (next p:allzero? (tail l)) false))))
(define p:paramnames (lambda (ps acc)
	(if (empty? ps) acc
	(next p:paramnames (tail ps) (p:cons (p:second (head ps)) acc)))))

(define p:globalvar (lambda (ty n attr q)
	(begin
		(define elem ty)
		(define size (p:arraysize))
		(define ty2 (if (= size nil) ty (+ ty p:PTR)))
		(define a2 (if (= size nil) attr (+ attr 64)))
		(define init
			(if (= (p:kind) 'Assign)
				(begin (p:advance) (p:init))
			nil))
		(define fsize (p:initsize size elem init "global"))
		;; a static const scalar with a constant initializer is a
		;; constant from here on, wherever C wants one (array sizes, case
		;; labels, other initializers). It keeps its storage.
		(if (if (= 8 (bit:and attr 8)) (if (= 1 (bit:and q 1))
				(if (= size nil) (if (< ty p:STRUCT0)
					(if (p:intty? ty) (if (= init nil) false (= (head init) 'num)) false)
				false) false) false) false)
			(set! p:sconsts (p:cons
				(list n (p:convval (p:second init) ty) (p:promote ty)) p:sconsts))
			nil)
		(p:qnode (list 'global ty2 n fsize a2 init)
			(if (= size nil) q (bit:shl q 2))))))

;; an integer value type (not a pointer, not a struct)
(define p:intty? (lambda (t)
	(if (< t 2) true
	(if (< t 512) false
	(if (>= t p:STRUCT0) false
	(= 0 (- (- t 512) (* 64 (/ (- t 512) 64)))))))))

;; the "while (tk != ';' && tk != '}')" declarator loop. A function
;; DEFINITION ends the line by itself: in c4cc its closing '}' is what
;; terminates the loop, and here p:block has already consumed it.
(define p:declarators (lambda (bt bq attr acc)
	(if (if (= (p:kind) 'Semi) true (= (p:kind) 'Rbrace))
		(begin (p:advance) acc)
	(begin
		(define d (p:declarator bt bq attr))
		(if (= (head d) 'func) (p:cons d acc)
		(begin
			(if (= (p:kind) 'Comma) (p:advance) nil)
			(next p:declarators bt bq attr (p:cons d acc))))))))

;; ---- program ----

;; queued structdef nodes enter the AST before the declaration that
;; introduced them (acc is reversed at the end, so oldest first here)
(define p:drain (lambda (acc)
	(if (empty? p:pending) acc
	(begin
		(define l (p:reverse p:pending))
		(set! p:pending (list))
		(next p:drain/2 l acc)))))
(define p:drain/2 (lambda (l acc)
	(if (empty? l) acc
	(next p:drain/2 (tail l) (p:cons (head l) acc)))))

(define p:typedefdecl (lambda ()
	(begin
		(p:advance)
		(define bt (p:basetype nil))
		(if (= bt nil) (p:die "typedef needs a type") nil)
		(define ty (p:stars bt p:bq))
		(define q p:sq)
		(if (= (p:kind) 'Id) nil (p:die "typedef needs a name"))
		(define n (p:value))
		(p:advance)
		(p:expect 'Semi "semicolon after typedef")
		(set! p:typedefs (p:cons (list n ty q) p:typedefs))
		(p:qnode (list 'typedefd n ty) q))))

(define p:top (lambda (acc)
	(if (= (p:kind) 'Eof) (p:reverse acc)
	(begin
		(define attr 0)
		(if (= (p:kind) 'Static)
			(begin (p:advance) (set! attr (+ attr 8))) nil)
		(if (= (p:kind) 'Extern)
			(begin (p:advance) (set! attr (+ attr 16))) nil)
		(if (= (p:kind) 'Enum)
			(begin
				(define ed (p:enumdecl))
				;; any declarators after the enum share INT base type
				(next p:top (p:declarators p:INT 0 attr (p:cons ed acc))))
		(if (= (p:kind) 'Typedef)
			(begin
				(define td (p:typedefdecl))
				(next p:top (p:cons td (p:drain acc))))
		(begin
			(define bt (p:basetype p:INT))
			(next p:top (p:declarators bt p:bq attr (p:drain acc))))))))))

(define parse:program (lambda (tokens)
	(begin
		(set! p:toks tokens)
		(set! p:enums (list))
		(set! p:sconsts (list))
		(set! p:locals (list))
		(set! p:structs (list))
		(set! p:nstructs 0)
		(set! p:typedefs (list))
		(set! p:pending (list))
		(p:cons 'program (p:top (list))))))

)
