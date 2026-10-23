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
;; long long on a 32-bit target (M8): two words; on a 64-bit target it is
;; the word, int and unsigned long as before
(define p:LL 896)
(define p:ULL 960)
;; float and double (docs/c4-float.md): a band of their own past any
;; struct id the parser hands out (struct k is 1024 + 64k), with the
;; same +2 per pointer level; c4lc-gen's g:FLOAT says what they are
(define p:FLOAT 1025024)
(define p:DOUBLE 1025088)
(define p:SIGN32 (bit:shl 1 31))
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
;; queues a (structdef NAME TYPE ISUNION MEMBERS SIZE LAYOUT) node on
;; p:pending. The layout is worked out here (M9), so a constant can say
;; sizeof of a struct, and c4lc-gen's g:structdef takes it as it is.
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
				(define ms (p:members (list)))
				(define lay (p:slayout ms isu 0 0 1 (list)))
				(define recs (p:reverse (p:second lay)))
				(set! p:slayouts (p:cons (list ty (head lay) recs (index lay 2)) p:slayouts))
				(set! p:pending (p:cons
					(list 'structdef n ty isu ms (head lay) recs)
					p:pending)))
			nil)
		ty)))

;; ---- layout (M9) ----
;;
;; A struct's size and its members' offsets, from the member list: C's
;; rule, as gcc lays a struct out. Each member starts at a multiple of its
;; own alignment -- 1 for the one-byte types, 2 for the shorts, the word
;; for the word-sized ones, min(8, word) for long long and double (as
;; i386's gcc does at 32 bits), a row's element's, a struct's own -- and
;; a struct is as aligned as its most aligned member, its size rounded up
;; to that. Records are c4lc-gen's: (NAME OFF TYPE BYTES Q), BYTES set for
;; an array or a struct member. (Until M9 every member took whole words.)
(define p:third (lambda (l) (index l 2)))
(define p:slayouts nil)   ;; ((TYPE SIZE RECORDS ALIGN) ...)
(define p:WMASK (- p:WORD 1))
(define p:slay (lambda (ty)
	(begin
		(define a (p:assoc ty p:slayouts))
		(if a a (p:die "a struct or union used before its definition")))))
;; the storage size of a value of type ty: c4lc-gen's g:tysize
(define p:tysize (lambda (ty)
	(if (p:svalue? ty) (p:second (p:slay ty))
	(p:ctysize ty))))
;; how far apart an array's elements are: c4lc-gen's g:estride. A row
;; or a struct is exactly its size, which its alignment already rounds
(define p:estride (lambda (ty)
	(begin
		(define s (p:tysize ty))
		(if (if (p:rowty? ty) true (p:svalue? ty)) s
		(if (< s p:WORD) s (* p:WORD (/ (+ s p:WMASK) p:WORD)))))))
;; the alignment of a value of type ty
(define p:talign (lambda (ty)
	(begin
		(define r (p:assoc ty p:rows))
		(if r (p:talign (p:second r))
		(if (p:svalue? ty) (index (p:slay ty) 3)
		(begin
			(define s (p:ctysize ty))
			(if (< s p:WORD) s p:WORD)))))))
;; #pragma pack(n) caps a member's alignment at n, and with it its
;; struct's: p:pack is the n in force, 0 for none. A struct keeps the
;; alignment it was laid out with.
(define p:pack 0)
(define p:packs nil)      ;; what pack(push) saved
(define p:palign (lambda (a) (if (= p:pack 0) a (if (< p:pack a) p:pack a))))
;; pack(n), pack(), pack(push[, n]) and pack(pop), as gcc takes them,
;; between declarations: a PragmaPack token and the line's parenthesized
;; arguments, from the lexer or the preprocessor
(define p:packargs (lambda (acc)
	(if (= (p:kind) 'Rparen) (begin (p:advance) (p:reverse acc))
	(if (= (p:kind) 'Eof) (p:die "unterminated #pragma pack")
	(begin
		(define k (p:kind))
		(define v (p:value))
		(p:advance)
		(if (= k 'Comma) (next p:packargs acc)
		(next p:packargs (p:cons v acc))))))))
(define p:pragmapack (lambda ()
	(begin
		(p:advance)
		(p:expect 'Lparen "( after #pragma pack")
		(define as (p:packargs (list)))
		(define a0 (if (empty? as) "" (+ "" (head as))))
		(if (empty? as) (set! p:pack 0)
		(if (= a0 "push")
			(begin
				(set! p:packs (p:cons p:pack p:packs))
				(if (empty? (tail as)) nil (set! p:pack (p:second as))))
		(if (= a0 "pop")
			(if (empty? p:packs) (set! p:pack 0)
			(begin
				(set! p:pack (head p:packs))
				(set! p:packs (tail p:packs))))
		(set! p:pack (head as))))))))
;; n rounded up to a multiple of a
(define p:alignup (lambda (n a) (* a (/ (+ n (- a 1)) a))))
;; returns (SIZE REVRECORDS ALIGN)
(define p:slayout (lambda (ms isu off mx al acc)
	(if (empty? ms)
		(list (p:alignup (if isu mx off) al) acc al)
	(begin
		(define m (head ms))           ;; (TYPE NAME ASIZE [Q])
		(define mty (head m))
		(define asz (p:third m))
		(define ety (if (= asz nil) mty (- mty p:PTR)))
		(define a (p:palign (p:talign ety)))
		(define at (if isu 0 (p:alignup off a)))
		(define bytes
			(if (= asz nil)
				(if (p:svalue? mty) (p:tysize mty) nil)
			(* asz (p:estride ety))))
		(define msize (if (= asz nil) (p:tysize mty) bytes))
		(define q (index m 3))
		(next p:slayout (tail ms) isu
			(if isu off (+ at msize))
			(if (> msize mx) msize mx)
			(if (> a al) a al)
			(p:cons (list (p:second m) at mty bytes (if (= q nil) 0 q)) acc))))))
;; the record of member n of struct type ty
(define p:smember (lambda (ty n)
	(begin
		(define r (p:findrec n (index (p:slay ty) 2)))
		(if r r (p:die (+ "no such member: " n))))))
(define p:findrec (lambda (n l)
	(if (empty? l) false
	(if (= (head (head l)) n) (head l)
	(next p:findrec n (tail l))))))
(define p:members (lambda (acc)
	(if (= (p:kind) 'Rbrace)
		(begin (p:advance) (p:reverse acc))
	(begin
		(define bt (p:basetype nil))
		(if (= bt nil) (p:die "bad struct member type") nil)
		(next p:members/2 bt p:bq acc)))))
(define p:members/2 (lambda (bt bq acc)
	(begin
		(define ty0 (p:stars bt bq))
		(define fp (if (p:fnptr?) (p:fnptrdecl) false))
		(define ty (if fp p:fnty ty0))
		(define q (if fp 0 p:sq))
		(if fp (if (p:second fp) nil (if (head fp) nil (p:die "bad struct member name")))
			(if (= (p:kind) 'Id) nil (p:die "bad struct member name")))
		(define n (if fp (head fp) (p:value)))
		(if fp nil (p:advance))
		(define size (if fp (p:second fp) (p:arraysize/t ty)))
		(define acc2 (p:cons
			(p:qnode (list (if (= size nil) ty (+ (p:elemty ty) p:PTR)) n size)
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
		(if (if (= k 'Char) true (= k 'Void)) (begin (p:advance) (next p:spec d q sg un sh lg 1 it bt))
		(if (= k 'Int) (begin (p:advance) (next p:spec d q sg un sh lg ch 1 bt))
		;; long double is double
		(if (= k 'Float) (begin (p:advance) (next p:spec d q sg un sh 0 ch it p:FLOAT))
		(if (= k 'Double) (begin (p:advance) (next p:spec d q sg un sh 0 ch it p:DOUBLE))
		(if (if bare (if (= k 'Struct) true (= k 'Union)) false)
			(begin
				(define st (p:structtype))
				(next p:spec d q sg un sh lg ch it st))
		;; enum [TAG] [{ ... }] is a type wherever one may be written
		;; (M7b): an int, whose constants the body defines on the way
		(if (if bare (= k 'Enum) false)
			(begin
				(p:enumspec)
				(next p:spec d q sg un sh lg ch 1 bt))
		(if (if bare (= k 'Id) false)
			(begin
				(define a (p:assoc (p:value) p:typedefs))
				(if a
					(begin
						(p:advance)
						(next p:spec d (bit:or q (index a 2)) sg un sh lg ch it (p:second a)))
				(p:specdone d q sg un sh lg ch it bt)))
		(p:specdone d q sg un sh lg ch it bt)))))))))))))))))
(define p:specdone (lambda (d q sg un sh lg ch it bt)
	(begin
		(set! p:bq q)
		(if (= bt nil)
			(if (= ch 1) (if (= un 1) p:UCHAR (if (= sg 1) p:SCHAR p:CHAR))
			(if (= sh 1) (if (= un 1) p:USHORT p:SHORT)
			(if (> lg 1)
				(if (= p:WORD 4) (if (= un 1) p:ULL p:LL) (if (= un 1) p:ULONG p:INT))
			(if (> lg 0) (if (= un 1) p:ULONG p:INT)
			(if (= un 1) p:UINT
			(if (= sg 1) p:INT
			(if (= it 1) p:INT
			d)))))))
		bt))))

;; is the current token the start of a declaration?
(define p:declstart? (lambda () (p:declstart-tok? (head p:toks))))
;; ... or any token, for a look ahead (sizeof's `(`)
(define p:declstart-tok? (lambda (tok)
	(begin
		(define k (head tok))
		(if (= k 'Int) true
		(if (= k 'Char) true
		(if (= k 'Void) true
		(if (= k 'Struct) true
		(if (= k 'Union) true
		(if (= k 'Enum) true
		(if (= k 'Const) true
		(if (= k 'Volatile) true
		(if (= k 'Signed) true
		(if (= k 'Unsigned) true
		(if (= k 'Short) true
		(if (= k 'Long) true
		(if (= k 'Float) true
		(if (= k 'Double) true
		(if (= k 'Id)
			(if (p:assoc (p:second tok) p:typedefs) true false)
		false))))))))))))))))))

;; consume the declarator's stars, each followed by its own qualifiers:
;; ty -> ty + 2 per star, and the mask q of the base moves up a level
;; per star. The declarator's mask is left in p:sq.
(define p:stars (lambda (ty q)
	(if (= (p:kind) 'Mul) (begin (p:advance) (next p:stars (+ ty p:PTR) (bit:shl q 2)))
	(if (= (p:kind) 'Const) (begin (p:advance) (next p:stars ty (bit:or q 1)))
	(if (= (p:kind) 'Volatile) (begin (p:advance) (next p:stars ty (bit:or q 2)))
	(begin (set! p:sq q) (set! p:lastty ty) ty))))))
;; the type p:stars made last: a function pointer declarator's return type
(define p:lastty 1)

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

;; sizeof inside a constant expression (M7b): of a type, of a string
;; literal, and (M9) of an expression over globals -- sizeof(table) /
;; sizeof(table[0]) -- worked out from their types as c4lc-gen's
;; g:sizeofx does. The rule is c4lc-tree's t:tysize, which is g:tysize's.
(define p:ctysize (lambda (ty)
	(if (= ty 0) 1
	(if (if (p:svalue? ty) true (p:rowty? ty)) (p:second (p:slay ty))
	(if (< ty 512) p:WORD
	(if (> (- (- ty 512) (* 64 (/ (- ty 512) 64))) 0) p:WORD
	(if (if (= ty 512) true (= ty 576)) 1
	(if (if (= ty 640) true (= ty 704)) 2
	(if (if (= ty 896) true (if (= ty 960) true (= ty p:DOUBLE))) 8
	p:WORD)))))))))
(define p:constsizeof (lambda (what)
	(begin
		(p:advance)
		(define v
			(if (if (= (p:kind) 'Lparen) (p:declstart-tok? (head (tail p:toks))) false)
				(begin
					(p:advance)
					(define bt (p:basetype p:INT))
					(define tv (p:ctysize (p:stars bt p:bq)))
					(p:expect 'Rparen ") after sizeof")
					tv)
			(p:csizeofx (p:expr 21) what)))
		(set! p:ct p:INT)
		v)))
;; the globals so far, for sizeof in a constant: ((NAME TYPE SIZE) ...),
;; TYPE as declared (an array's is its element's plus a pointer level)
;; and SIZE its elements, or nil for a scalar
(define p:gvars nil)
;; ... and the current function's locals, in the same form (M12): a local
;; array's size is as constant as a global's, `menuParents[sizeof(menus) /
;; sizeof *(menus)]`
(define p:lvars (list))
(define p:gvar (lambda (n what)
	(begin
		(define lv (p:assoc n p:lvars))
		(define g (if lv lv (if (p:member? n p:locals) false (p:assoc n p:gvars))))
		(if g g (p:die (+ what (+ ": sizeof of " (+ n " is not a constant here"))))))))
;; the size of what expression e names, without evaluating it
(define p:csizeofx (lambda (e what)
	(begin
		(define h (head e))
		(if (= h 'str) (+ (length (p:second e)) 1)
		(if (= h 'var)
			(begin
				(define g (p:gvar (p:second e) what))
				(if (= (p:third g) nil) (p:tysize (p:second g))
				(if (= (p:third g) 0) (p:die (+ what (+ ": the size of " (+ (p:second e) " is not known here"))))
				(* (p:third g) (p:estride (- (p:second g) p:PTR))))))
		(if (if (= h 'member) true (= h 'arrow))
			(begin
				(define r (p:membrec e what))
				(if (= (index r 3) nil) (p:tysize (p:third r)) (index r 3)))
		(p:tysize (p:ctyof e what))))))))
;; the record of the member a member or arrow node names
(define p:membrec (lambda (e what)
	(begin
		(define bt (p:ctyof (p:second e) what))
		(p:smember (if (= (head e) 'arrow) (- bt p:PTR) bt) (p:third e)))))
;; the type of expression e, for sizeof
(define p:ctyof (lambda (e what)
	(begin
		(define h (head e))
		(if (= h 'var) (p:second (p:gvar (p:second e) what))
		(if (if (= h 'index) true (= h 'deref)) (- (p:ctyof (p:second e) what) p:PTR)
		(if (if (= h 'member) true (= h 'arrow)) (p:third (p:membrec e what))
		(if (= h 'cast) (p:second e)
		(if (= h 'str) p:PTR
		(p:die (+ what ": sizeof of this expression is not a constant here"))))))))))

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
	(if (= (p:kind) 'Sizeof) (p:constsizeof what)
	(if (if (= (p:kind) 'Lparen) (p:declstart-tok? (head (tail p:toks))) false)
		(p:constcast what)
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
	(p:die (+ what " must be an integer constant")))))))))))))

;; a cast in a constant, `case (Uint32)KMOD_SHIFT << 16:` (step 4 of
;; docs/ports-feasibility.md §8): the operand converted as the generated
;; code converts it. A pointer type keeps the value, as the word it is.
(define p:constcast (lambda (what)
	(begin
		(p:advance)
		(define bt (p:basetype p:INT))
		(define ty0 (p:stars bt p:bq))
		(define ty (if (p:fnptr?) (begin (p:fnptrdecl) p:fnty) ty0))
		(p:expect 'Rparen ") after a cast")
		(define v (p:constatom what))
		(if (if (p:svalue? ty) true (if (= ty p:FLOAT) true (= ty p:DOUBLE)))
			(p:die (+ what " must be an integer constant")) nil)
		(set! p:ct (if (p:intty? ty) (if (= ty p:ULL) p:ULONG (if (= ty p:LL) p:INT ty)) p:INT))
		(if (p:intty? ty) (p:convval v ty) v))))

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

;; comparisons (M7b): C's order, relational above equality above &
(define p:constrel (lambda (what)
	(begin (define v (p:constshift what)) (next p:constrel/2 what v p:ct))))
(define p:constrel/2 (lambda (what l lt)
	(begin
		(define op (p:assoc/atom (p:kind) '((Lt lt) (Gt gt) (Le le) (Ge ge))))
		(if op
			(begin (p:advance) (define r (p:constshift what))
				(define v (p:cstep (p:second op) l lt r)) (next p:constrel/2 what v p:ct))
		(begin (set! p:ct lt) l)))))
(define p:consteq (lambda (what)
	(begin (define v (p:constrel what)) (next p:consteq/2 what v p:ct))))
(define p:consteq/2 (lambda (what l lt)
	(begin
		(define op (p:assoc/atom (p:kind) '((Eq eq) (Ne ne))))
		(if op
			(begin (p:advance) (define r (p:constrel what))
				(define v (p:cstep (p:second op) l lt r)) (next p:consteq/2 what v p:ct))
		(begin (set! p:ct lt) l)))))

(define p:constand (lambda (what)
	(begin (define v (p:consteq what)) (next p:constand/2 what v p:ct))))
(define p:constand/2 (lambda (what l lt)
	(if (= (p:kind) 'And)
		(begin (p:advance) (define r (p:consteq what))
			(define v (p:cstep 'band l lt r)) (next p:constand/2 what v p:ct))
	(begin (set! p:ct lt) l))))

(define p:constxor (lambda (what)
	(begin (define v (p:constand what)) (next p:constxor/2 what v p:ct))))
(define p:constxor/2 (lambda (what l lt)
	(if (= (p:kind) 'Xor)
		(begin (p:advance) (define r (p:constand what))
			(define v (p:cstep 'bxor l lt r)) (next p:constxor/2 what v p:ct))
	(begin (set! p:ct lt) l))))

(define p:constbor (lambda (what)
	(begin (define v (p:constxor what)) (next p:constbor/2 what v p:ct))))
(define p:constbor/2 (lambda (what l lt)
	(if (= (p:kind) 'Or)
		(begin (p:advance) (define r (p:constxor what))
			(define v (p:cstep 'bor l lt r)) (next p:constbor/2 what v p:ct))
	(begin (set! p:ct lt) l))))

;; && and || (M7b), both sides always read: a constant has no side
;; effects to skip, and folding cannot trap (p:sarith's / of 0 aside,
;; which a well-formed constant does not reach)
(define p:constland (lambda (what)
	(begin (define v (p:constbor what)) (next p:constland/2 what v))))
(define p:constland/2 (lambda (what l)
	(if (= (p:kind) 'Lan)
		(begin (p:advance) (define r (p:constbor what)) (set! p:ct p:INT)
			(next p:constland/2 what (if (if (= l 0) false (if (= r 0) false true)) 1 0)))
	l)))
(define p:constlor (lambda (what)
	(begin (define v (p:constland what)) (next p:constlor/2 what v))))
(define p:constlor/2 (lambda (what l)
	(if (= (p:kind) 'Lor)
		(begin (p:advance) (define r (p:constland what)) (set! p:ct p:INT)
			(next p:constlor/2 what (if (if (= l 0) (= r 0) false) 0 1)))
	l)))

;; the whole constant expression, with ?: at the top (M7b): Busybox's
;; libbb.h sizes its compile-time checks as `cond ? 1 : -1`
(define p:const (lambda (what)
	(begin
		(define c (p:constlor what))
		(if (= (p:kind) 'Cond)
			(begin
				(p:advance)
				(define a (p:const what))
				(define at p:ct)
				(p:expect 'Colon "colon in constant conditional")
				(define b (p:const what))
				(define bt p:ct)
				(set! p:ct (if (= c 0) bt at))
				(if (= c 0) b a))
		c))))

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

;; sizeof of a type in parentheses (struct/union/typedef names too), or
;; of any unary expression, parenthesized or not (M7b). An identifier
;; stays (sizeofa NAME): its array storage, or its type's size. Any other
;; operand is (sizeofe E), which the generator types without evaluating.
;;
;; `sizeof(T[n])` is sizeof(T) * n: Busybox's BUILD_BUG_ON is written
;; ((void)sizeof(char[1 - 2*!!(cond)])), in function bodies, where the
;; product is an ordinary expression for -O to fold.
(define p:sizeof (lambda ()
	(begin
		(p:advance)
		(if (if (= (p:kind) 'Lparen) (p:declstart-tok? (head (tail p:toks))) false)
			(begin
				(p:advance)
				(define bt (p:basetype p:INT))
				(define ty (p:stars bt p:bq))
				(if (= (p:kind) 'Brak)
					(begin
						(p:advance)
						(define n (p:expr 1))
						(p:expect 'Rbrak "] in sizeof")
						(p:expect 'Rparen ") after sizeof")
						(list 'mul (list 'sizeof ty) n))
				(begin
					(p:expect 'Rparen ") after sizeof")
					(list 'sizeof ty))))
		(begin
			(define e (p:expr 21))
			(if (= (head e) 'var) (list 'sizeofa (p:second e))
			(list 'sizeofe e)))))))

;; parenthesized: cast, or (expr[, expr...]). A cast begins with a
;; type keyword or a typedef name -- the registries disambiguate
;; (T)x from a parenthesized variable.
(define p:parenexpr (lambda ()
	(begin
		(p:advance)
		(if (p:declstart?)
			(begin
				(define bt (p:basetype p:INT))
				(define ty0 (p:stars bt p:bq))
				(define fp (if (p:fnptr?) (p:fnptrdecl) false))
				(define ty (if fp p:fnty ty0))
				(define cq (if fp 0 p:sq))
				;; (T[N]){ ... }: a compound literal of an array type
				(define asz (if fp nil (p:arraysize)))
				(p:expect 'Rparen "closing paren of cast")
				(if (if (= asz nil) (= (p:kind) 'Lbrace) true)
					(p:cmpdlit ty asz)
				(p:qnode (list 'cast ty (p:expr 21)) cq)))
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

;; ---- compound literals (M12) ----
;;
;; (T){ ... } and (T[N]){ ... }: an object of type T made where the
;; expression is. It is a hidden local of the function, $clN, declared
;; with the others; the expression initializes it from its brace list each
;; time it is reached (g:cmpd), and its value is the local's -- a struct's
;; or an array's address, as a named one's would be. A function that makes
;; one is never inlined (attribute 128): the inliner renames the names it
;; knows, and this is one it does not. OpenTyrian writes seven, five of
;; them as `return (MemReader){ data, size, false };`.
(define p:hiddens (list))
(define p:ncl 0)
(define p:infn false)
(define p:cmpdlit (lambda (ty asz)
	(begin
		(if p:infn nil (p:die "a compound literal outside a function is not supported"))
		(if (= (p:kind) 'Lbrace) nil (p:die "a brace list after the type of a compound literal"))
		(define n (+ "$cl" (+ "" p:ncl)))
		(set! p:ncl (+ p:ncl 1))
		(define elem (if (= asz nil) ty (p:elemty ty)))
		(define ty2 (if (= asz nil) ty (+ elem p:PTR)))
		(define init (p:linit))
		(define fsize (p:initsize asz elem init "compound literal"))
		(set! p:hiddens (p:cons (list 'local ty2 n fsize nil) p:hiddens))
		(list 'cmpd n ty2 fsize init))))

;; A full expression, the comma operator and all: what a statement, a
;; for clause, a condition and a return value are. Inside parentheses
;; p:parenexpr takes the commas itself, and an argument list or an
;; initializer takes p:expr 1, which stops at one.
(define p:fullexpr (lambda ()
	(begin
		(define e1 (p:expr 1))
		(if (= (p:kind) 'Comma)
			(p:cons 'comma (p:commalist (list e1)))
		e1))))

;; primary / unary
(define p:primary (lambda ()
	(begin
		(define k (p:kind))
		(if (= k 'Num) (begin (define n (p:value)) (p:advance) (list 'num n))
		(if (p:lltok? k) (p:llnumtok)
		(if (p:fptok? k) (p:fplit false)
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
		(p:die "bad expression"))))))))))))))))))))
;; a literal too wide for a 32-bit word (c4lc-lex): (llnum LO HI TYPE)
(define p:lltok? (lambda (k) (if (= k 'NumLL) true (= k 'NumULL))))
(define p:llnumtok (lambda ()
	(begin
		(define k (p:kind))
		(define v (p:value))
		(p:advance)
		(list 'llnum (head v) (p:second v) (if (= k 'NumULL) p:ULL p:LL)))))

;; unary minus: fold a literal, as c4cc does
(define p:negexpr (lambda ()
	(begin
		(p:advance)
		(if (= (p:kind) 'Num)
			(begin
				(define n (p:value))
				(p:advance)
				(list 'num (- 0 n)))
		(if (p:fptok? (p:kind)) (p:fplit true)
		(list 'neg (p:expr 21)))))))

;; identifier: call, enum constant, or variable. A static local's name
;; is its global's (p:srename). __func__ is the function's name (M7b).
(define p:funcname? (lambda (n)
	(if (= n "__func__") true (if (= n "__FUNCTION__") true (= n "__PRETTY_FUNCTION__")))))
(define p:idexpr (lambda ()
	(if (p:funcname? (p:value))
		(begin (p:advance) (list 'str p:curfn))
	(begin
		(define n (p:srename (p:value)))
		(define ev (p:enumval))
		(p:advance)
		(if (= (p:kind) 'Lparen)
			(begin
				(p:advance)
				(p:cons 'call (p:cons n (p:args (list)))))
		(if (= ev false) (list 'var n)
		(list 'num ev)))))))

;; precedence climbing over a parsed left node
(define p:expr (lambda (lev)
	(p:climb (p:primary) lev)))

(define p:climb (lambda (lhs lev)
	(begin
		(define k (p:kind))
		;; '(' after an expression is a call, binding as tightly as [ ]
		(define l (if (= k 'Lparen) 23 (p:level k)))
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
		;; a call through any expression (M7b): ops[i](x), s->fn(x),
		;; (*fp)(x). A name followed by '(' never gets here: p:idexpr
		;; made it a call. (*fp)(x) calls fp, as C says -- which the
		;; generator sees to, knowing whether fp is a pointer to one (M13)
		(if (= k 'Lparen)
			(begin
				(p:advance)
				(define tgt lhs)
				(next p:climb (p:cons 'calle (p:cons tgt (p:args (list)))) lev))
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
						lev))))))))))))))))))

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
		;; goto and labels (M7b). `goto` is no keyword in this lexer or
		;; c4fc's, so the statement is known by its shape: goto NAME ;
		(if (if (= k 'Id) (if (= (p:value) "goto") (= (head (head (tail p:toks))) 'Id) false) false)
			(begin
				(p:advance)
				(define gn (p:value))
				(p:advance)
				(p:expect 'Semi "semicolon after goto")
				(list 'goto gn))
		;; NAME : statement. A label just before the closing brace (C23
		;; allows it, and Busybox writes `out: ; }` and `out: }` both)
		;; labels an empty statement.
		(if (if (= k 'Id) (= (head (head (tail p:toks))) 'Colon) false)
			(begin
				(define ln (p:value))
				(p:advance)
				(p:advance)
				(list 'labeled ln (if (= (p:kind) 'Rbrace) '(empty) (p:stmt))))
		(if (= k 'Typedef) (begin (p:typedefdecl) '(empty))
		(if (if (p:declstart?) true (if (= k 'Static) true (= k 'Extern)))
			(p:cons 'declstmt (p:localline))
		(begin
			(define e (p:fullexpr))
			(p:expect 'Semi "semicolon")
			(list 'expr e)))))))))))))))))))

(define p:dostmt (lambda ()
	(begin
		(p:advance)
		(define s (p:stmt))
		(p:expect 'While "while after do body")
		(p:expect 'Lparen "open paren after do-while")
		(define c (p:fullexpr))
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
		(define c (p:fullexpr))
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
		(define c (p:fullexpr))
		(p:expect 'Rparen "close paren after while condition")
		(list 'while c (p:stmt)))))

;; for (init; cond; step) stmt -- parsed correctly (see header); each
;; part may be empty
(define p:forstmt (lambda ()
	(begin
		(p:advance)
		(p:expect 'Lparen "open paren after for")
		;; for (int i = 0; ...) (M12): C99's declaration, scoped to the
		;; loop -- a block around it, holding the declaration and a for
		;; with no initializer. Its ';' is the declaration's own.
		(if (p:declstart?)
			(begin
				(define d (p:cons 'declstmt (p:localline)))
				(list 'block d (p:forstmt/2 nil)))
		(begin
			(define i (if (= (p:kind) 'Semi) nil (p:fullexpr)))
			(p:expect 'Semi "semicolon after for initializer")
			(p:forstmt/2 i))))))
(define p:forstmt/2 (lambda (i)
	(begin
		(define c (if (= (p:kind) 'Semi) nil (p:fullexpr)))
		(p:expect 'Semi "semicolon after for condition")
		(define s (if (= (p:kind) 'Rparen) nil (p:fullexpr)))
		(p:expect 'Rparen "close paren after for")
		(list 'for i c s (p:stmt)))))

(define p:returnstmt (lambda ()
	(begin
		(p:advance)
		(if (= (p:kind) 'Semi)
			(begin (p:advance) (list 'return nil))
		(begin
			(define e (p:fullexpr))
			(p:expect 'Semi "semicolon after return")
			(list 'return e))))))

;; switch (expr) { case C: ... default: ... }: case/default only at
;; the top level of the switch braces, exactly as c4cc
(define p:switchstmt (lambda ()
	(begin
		(p:advance)
		(p:expect 'Lparen "open paren after switch")
		(define c (p:fullexpr))
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
;; ---- function-pointer declarators (M7b) ----
;;
;; `T (*name)(params)`, `T (*name[N])(params)`, and with no name in a
;; cast, a parameter or a sizeof. c4 calls through any word -- a
;; function's value is its address, and an `int` holding one can be
;; called -- so the declarator declares an int, or an array of them. The
;; parameter list is skipped token by token (c4fc does the same), so a
;; struct first named inside it is not given an id here and not in c4fc.
;; A function pointer declarator's type: a word, as every function pointer
;; is here, and a pointer to one for each further * (M13): Rise of the
;; Triad's movement_status (**movement_function)(objtype*,int,int,int);
(define p:fnty 1)
(define p:fnstars (lambda (t)
	(if (= (p:kind) 'Mul) (begin (p:advance) (next p:fnstars (+ t p:PTR))) t)))
(define p:fnptr? (lambda ()
	(if (= (p:kind) 'Lparen) (= (head (head (tail p:toks))) 'Mul) false)))
;; consumes `( * [qualifiers] [NAME] [[N]] ) ( ... )`; returns (NAME SIZE),
;; NAME false when there is none and SIZE nil when it is no array
(define p:fnptrdecl (lambda ()
	(begin
		;; a call through the pointer has no type but the word's, so a float
		;; or double coming back would be read as an integer -- and a double
		;; on a 32-bit target returns through a pointer the call never passes
		(if (if (= p:lastty p:FLOAT) true (= p:lastty p:DOUBLE))
			(p:die "a pointer to a function returning float or double is not supported yet (docs/c4-float.md)")
			nil)		(p:advance)
		(p:advance)
		(set! p:fnty (p:fnstars p:INT))
		(p:skipquals)
		(define n (if (= (p:kind) 'Id) (p:value) false))
		(if n (p:advance) nil)
		(define size (p:arraysize))
		(set! p:inner nil)
		(p:expect 'Rparen ") in a function pointer declarator")
		(if (= (p:kind) 'Lparen) nil (p:die "parameter list of a function pointer expected"))
		(p:skipparens 0)
		(list n size))))
(define p:skipquals (lambda ()
	(if (if (= (p:kind) 'Const) true (= (p:kind) 'Volatile))
		(begin (p:advance) (next p:skipquals))
	nil)))
;; from a '(' to its matching ')', inclusive
(define p:skipparens (lambda (d)
	(begin
		(define k (p:kind))
		(if (= k 'Eof) (p:die "unterminated parameter list")
		(begin
			(p:advance)
			(if (= k 'Lparen) (next p:skipparens (+ d 1))
			(if (= k 'Rparen) (if (= d 1) nil (next p:skipparens (- d 1)))
			(next p:skipparens d))))))))

(define p:arraysize (lambda ()
	(if (= (p:kind) 'Brak)
		(begin
			(p:advance)
			(define s
				(if (= (p:kind) 'Rbrak) 0
				(p:const "array size")))
			(p:expect 'Rbrak "] after array size")
			(set! p:inner (p:innerdims (list)))
			s)
	nil)))

;; ---- arrays of arrays (M9) ----
;;
;; T a[N][M] is an array of N elements of a ROW type: M elements of T, of
;; exactly M times T's stride. A row is a type of its own in the struct
;; band, with no members. p:arraysize returns N and leaves the dimensions
;; after it in p:inner; p:elemty makes the element type of them, which
;; every declarator that took an array size uses in place of T. A row
;; queues a structdef node, (structdef "@row" TYPE false () SIZE () ELEM
;; COUNT), for the generator, which lets a row stand for the address of
;; its first element wherever its value is used.
(define p:inner nil)
(define p:innerdims (lambda (acc)
	(if (= (p:kind) 'Brak)
		(begin
			(p:advance)
			(define s (p:const "array size"))
			(p:expect 'Rbrak "] after array size")
			(next p:innerdims (p:cons s acc)))
	(p:reverse acc))))
(define p:elemty (lambda (ty)
	(if p:rowelem
		(begin
			(define e p:rowelem)
			(set! p:rowelem nil)
			e)
	(begin
		(define ds p:inner)
		(set! p:inner nil)
		(p:rowwrap ty (p:reverse ds))))))

;; ... and a declarator of an array typedef's type, a row (ports-feasibility
;; §8 step 6), with no brackets of its own: `digest_t d;` is the row's
;; element `d[N]`, laid out, indexed and passed as that array is. The size
;; is the row's count, and p:elemty gives the row's element.
(define p:rowelem nil)
(define p:arraysize/t (lambda (ty)
	(begin
		(define s (p:arraysize))
		(if (= s nil)
			(begin
				(define r (p:assoc ty p:rows))
				(if r
					(begin
						(set! p:rowelem (p:second r))
						(index r 2))
					nil))
			s))))
(define p:rowwrap (lambda (ty ds)
	(if (empty? ds) ty (next p:rowwrap (p:rowtype ty (head ds)) (tail ds)))))
(define p:rows nil)   ;; ((TYPE ELEM COUNT) ...)
(define p:rowtype (lambda (elem n)
	(begin
		(define r (p:findrow elem n p:rows))
		(if r (head r)
		(begin
			(define ty (+ p:STRUCT0 (* p:SSTEP p:nstructs)))
			(set! p:nstructs (+ p:nstructs 1))
			(define size (* n (p:estride elem)))
			(set! p:rows (p:cons (list ty elem n) p:rows))
			(set! p:slayouts (p:cons (list ty size (list) (p:talign elem)) p:slayouts))
			(set! p:pending (p:cons
				(list 'structdef "@row" ty false (list) size (list) elem n) p:pending))
			ty)))))
(define p:findrow (lambda (elem n l)
	(if (empty? l) false
	(if (if (= (p:second (head l)) elem) (= (index (head l) 2) n) false) (head l)
	(next p:findrow elem n (tail l))))))
(define p:rowty? (lambda (ty) (if (p:assoc ty p:rows) true false)))
;; the scalars one element of type ty holds, for brace elision
(define p:scalars (lambda (ty)
	(begin
		(define r (p:assoc ty p:rows))
		(if r (* (index r 2) (p:scalars (p:second r))) 1))))

;; initializer after '=': (num N) | (str S) | (fnaddr F) | (name F) |
;; (braces (N...)) | (agg ITEMS).
;;
;; M7b. A brace list of plain numbers keeps the (braces (N ...)) form that
;; every existing user takes. Anything else is (agg ITEMS): an item is an
;; initializer, (desig I INIT) for `[I] = INIT` or (mdesig M INIT) for
;; `.M = INIT`, and a string concatenates as an expression's does. A
;; bare name that is no constant is (name F), which the generator wants
;; to be a function -- the address in an applet or builtin table -- or a
;; global, an array's address; `&F` is (fnaddr F), either too. The
;; layout by type is c4lc-gen's g:globalaggr.
(define p:init (lambda ()
	(begin
		(define k (p:kind))
		(if (= k 'Lbrace) (p:braceinit)
		(if (p:lltok? k) (p:llnumtok)
		(if (if p:inlocal (p:exprleaf?) false) (list 'expr (p:expr 1))
		(if (if p:inlocal false (p:foldleaf?)) (p:foldinit)
		(if (p:fptok? k) (p:fplit false)
		(if (if (= k 'Sub) (p:fptok? (head (head (tail p:toks)))) false) (begin (p:advance) (p:fplit true))
		(if (= k 'Str) (p:strlit)
		(if (= k 'And)
			(begin
				(p:advance)
				(if (= (p:kind) 'Id)
					(begin
						(define n (p:value))
						(p:advance)
						(list 'fnaddr n))
				(p:die "& initializer requires a name")))
		(if (p:initname?)
			(begin
				(define fn (p:value))
				(p:advance)
				(list 'name fn))
		(list 'num (p:const "initializer"))))))))))))))
;; A local's brace list (M7b): as a global's, but a leaf that is no
;; constant is (expr E), which c4lc-gen's g:localaggr stores when the
;; declaration runs. A leaf is an expression when it starts with &, or
;; when before the next , or } there is a string, a type name, or a name
;; that is no enum or static const constant; a string to start is a
;; string. c4fc's LOCAL-BRACES and EXPR-LEAF?.
(define p:inlocal false)
(define p:linit (lambda ()
	(begin
		(set! p:inlocal true)
		(define r (p:braceinit))
		(set! p:inlocal false)
		r)))
(define p:exprleaf? (lambda ()
	(if (= (p:kind) 'Str) false
	(if (= (p:kind) 'And) true
	(p:exprleaf?/2 p:toks 0)))))
(define p:exprleaf?/2 (lambda (ts d)
	(begin
		(define t (head ts))
		(define k (head t))
		(if (= k 'Eof) false
		(if (if (= d 0) (if (= k 'Comma) true (if (= k 'Rbrace) true (= k 'Semi))) false) false
		(if (if (= k 'Str) true (p:fptok? k)) true
		(if (p:declstart-tok? t) true
		(if (if (= k 'Id) (= (p:constname? (p:second t)) false) false) true
		(next p:exprleaf?/2 (tail ts)
			(if (if (= k 'Lparen) true (= k 'Brak)) (+ d 1)
			(if (if (= k 'Rparen) true (= k 'Rbrak)) (- d 1) d)))))))))))
(define p:constname? (lambda (n)
	(if (p:member? n p:locals) false
	(if (p:assoc n p:enums) true
	(if (p:assoc n p:sconsts) true false)))))
;; a name standing alone in an initializer that no constant answers
(define p:initname? (lambda ()
	(if (= (p:kind) 'Id)
		;; an enum constant's value may be 0, which is no test of truth
		(if (= (p:enumval) false) (if (p:sconst) false
		(begin
			(define nk (head (head (tail p:toks))))
			(if (= nk 'Comma) true (if (= nk 'Rbrace) true (= nk 'Semi))))))
	false)))
(define p:braceinit (lambda ()
	(begin
		(p:advance)
		(next p:braceinit/2 (list)))))
(define p:braceinit/2 (lambda (acc)
	(if (= (p:kind) 'Rbrace)
		(begin (p:advance) (p:bracelist (p:reverse acc)))
	(begin
		(define v (p:initem))
		(if (= (p:kind) 'Comma) (p:advance) nil)
		(next p:braceinit/2 (p:cons v acc))))))
(define p:initem (lambda ()
	(if (= (p:kind) 'Brak)
		(begin
			(p:advance)
			(define c (p:const "array designator"))
			(p:expect 'Rbrak "] in a designator")
			(p:expect 'Assign "= after a designator")
			(list 'desig c (p:init)))
	(if (if (= (p:kind) 'Dot) (= (head (head (tail p:toks))) 'Id) false)
		(begin
			(p:advance)
			(define m (p:value))
			(p:advance)
			(p:expect 'Assign "= after a designator")
			(list 'mdesig m (p:init)))
	(p:init)))))
(define p:bracelist (lambda (items)
	(if (p:allnum? items) (list 'braces (p:nums items (list))) (list 'agg items))))
(define p:allnum? (lambda (l)
	(if (empty? l) true
	(if (if (= (head (head l)) 'num) (not (p:fpnum? (head l))) false) (next p:allnum? (tail l)) false))))
(define p:nums (lambda (l acc)
	(if (empty? l) (p:reverse acc) (next p:nums (tail l) (p:cons (p:second (head l)) acc)))))
;; how many elements an (agg ...) list fills: designators move the index
(define p:aggcount (lambda (l i mx)
	(if (empty? l) mx
	(begin
		(define it (head l))
		(define j (if (= (head it) 'desig) (p:second it) i))
		(next p:aggcount (tail l) (+ j 1) (if (> (+ j 1) mx) (+ j 1) mx))))))

;; resolve an array's element count from its initializer when [] was
;; given; also the implicit size of char t[] = "...". Returns the
;; final SIZE for the AST node (nil = scalar).
(define p:initsize (lambda (size elem init what)
	(if (= size nil) nil
	(if (> size 0) size
	(if (= init nil) (p:die (+ what ": array [] needs an initializer"))
	(if (= (head init) 'braces)
		(begin
			(define k (p:scalars elem))
			(/ (+ (length (p:second init)) (- k 1)) k))
	(if (= (head init) 'agg) (p:aggcount (p:second init) 0 0)
	(if (if (= (head init) 'str) (p:bytety? elem) false)
		(+ (length (p:second init)) 1)
	(p:die (+ what ": cannot size array from this initializer"))))))))))

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
;; enum as a type specifier: the keyword, an optional tag, an optional
;; body, whose constants p:enumbody defines
(define p:enumspec (lambda ()
	(begin
		(p:advance)
		(if (= (p:kind) 'Id) (p:advance) nil)
		(if (= (p:kind) 'Lbrace) (begin (p:advance) (p:enumbody 0 (list))) nil))))
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
;;
;; `(void)` is no parameters at all. A parameter may have no name, as in
;; a prototype's `int f(int, char *)`; it gets one no source can spell.
(define p:params (lambda (acc)
	(begin
		(define k (p:kind))
		(if (= k 'Rparen)
			(begin (p:advance) (list (p:reverse acc) false))
		(if (if (= k 'Void) (if (empty? acc) (= (head (head (tail p:toks))) 'Rparen) false) false)
			(begin (p:advance) (p:advance) (list (p:reverse acc) false))
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
			(define ty0 (p:stars bt p:bq))
			(define fp (if (p:fnptr?) (p:fnptrdecl) false))
			(define ty1 (if fp p:fnty ty0))
			(define q1 (if fp 0 p:sq))
			(define named (if fp (if (head fp) true false) (= (p:kind) 'Id)))
			(define n (if named (if fp (head fp) (p:value)) (+ " param" (+ "" (length acc)))))
			(if (if named (if fp false true) false) (p:advance) nil)
			;; T name[] or T name[N]: C's parameter is a pointer to T; and
			;; so is one of an array typedef's type (p:arraysize/t)
			(define asz (if fp nil (p:arraysize/t ty1)))
			(define ty (if (= asz nil) ty1 (+ (p:elemty ty1) p:PTR)))
			(define q (if (= asz nil) q1 (bit:shl q1 2)))
			(if (if (= (p:kind) 'Comma) true (= (p:kind) 'Rparen)) nil
				(p:die "bad parameter declaration"))
			(if (= (p:kind) 'Comma) (p:advance) nil)
			(next p:params (p:cons (p:qnode (list ty n) q) acc)))))))))

;; ---- static locals (M7b) ----
;;
;; A static local is a global named FUNCTION.NAME -- no C identifier has a
;; '.', so it can clash with nothing -- declared just before its function
;; (p:hoist), so its data lands where c4fc's, declared in the body, does.
;; Inside the function the plain name means it (p:srenames). T2 drops it
;; with a dead function (c4lc-tree's t:dropdead).
(define p:curfn "")
(define p:srenames (list))
(define p:srename (lambda (n)
	(begin
		(define r (p:assoc n p:srenames))
		(if r (p:second r) n))))
(define p:statics (lambda (bt bq)
	(begin
		(define d (p:declarator bt bq 8))
		(if (= (head d) 'global) nil (p:die "a static local must be a variable"))
		(define n (index d 2))
		(define m (+ (+ p:curfn ".") n))
		;; its size for sizeof in a constant, under the name it now has (M12)
		(define ge (p:assoc n p:gvars))
		(if ge (set! p:gvars (p:cons (list m (p:second ge) (p:third ge)) p:gvars)) nil)
		(if (if (empty? p:dyninits) false (= (head (head p:dyninits)) n))
			(set! p:dyninits (p:cons (list m (p:second (head p:dyninits)) (index (head p:dyninits) 2)) (tail p:dyninits)))
			nil)
		(set! p:srenames (p:cons (list n m) p:srenames))
		(set! p:pending (p:cons
			(+ (list (head d) (p:second d) m) (tail (tail (tail d)))) p:pending))
		(if (= (p:kind) 'Comma) (begin (p:advance) (next p:statics bt bq))
		(p:expect 'Semi "semicolon after a static declaration")))))
;; a function and the static locals its body queued: they go first
(define p:hoist (lambda (acc)
	(if (empty? p:pending) acc
	(if (empty? acc) acc
	(if (= (head (head acc)) 'func) (p:cons (head acc) (p:drain (tail acc)))
	acc)))))

;; ---- local declarations ----

;; one "int|char decl, decl;" line; returns local nodes (order kept).
;; A static line returns none: its globals are queued (p:statics). So
;; does an extern line, and a function's prototype among the locals.
(define p:localline (lambda ()
	(if (= (p:kind) 'Static)
		(begin
			(p:advance)
			(define sbt (p:basetype p:INT))
			(p:statics sbt p:bq)
			(list))
	(if (= (p:kind) 'Extern)
		(begin
			(p:advance)
			(define ebt (p:basetype p:INT))
			(p:blockexterns ebt p:bq)
			(list))
	(begin
		(define bt (p:basetype p:INT))
		(next p:localline/2 bt p:bq (list)))))))
;; ---- declarations of globals inside a block ----
;;
;; `extern int forwardmove[2];` and `void V_MarkRect(int, int, int, int);`
;; in a function body (Doom writes both) declare what the file scope
;; declares: they go just before the function, as a static local's global
;; does (p:hoist). A wider scope than C gives them is harmless -- they
;; allocate nothing. c4fc's LOCAL-DECLS declares them in its file scope.
(define p:blockexterns (lambda (bt bq)
	(begin
		(define d (p:declarator bt bq 16))
		(if (= (head d) 'func) (p:die "a function cannot be defined inside a block") nil)
		(set! p:pending (p:cons d p:pending))
		(if (= (p:kind) 'Comma) (begin (p:advance) (next p:blockexterns bt bq))
		(p:expect 'Semi "semicolon after an extern declaration")))))
(define p:localline/2 (lambda (bt bq acc)
	(if (= (p:kind) 'Semi)
		(begin (p:advance) (p:reverse acc))
	(begin
		(define ty0 (p:stars bt bq))
		(p:unparen)
		(if (if (= (p:kind) 'Id) (= (head (head (tail p:toks))) 'Lparen) false)
			(next p:localproto ty0 bt bq acc)
			(next p:localline/3 ty0 bt bq acc))))))
;; `T (name)`: a declarator's name in parentheses, as a macro writes its
;; parameter (M12; OpenTyrian's foreach_option_i_value declares
;; `unsigned int (i)` and `const char *(string_value)`). The parentheses
;; go, and the name is read as usual.
(define p:unparen (lambda ()
	(if (if (= (p:kind) 'Lparen)
			(if (= (head (head (tail p:toks))) 'Id) (= (head (head (tail (tail p:toks)))) 'Rparen) false)
			false)
		(set! p:toks (p:cons (head (tail p:toks)) (tail (tail (tail p:toks)))))
		nil)))
;; a prototype among the locals, `T name(params);`: queued as
;; p:blockexterns queues one
(define p:localproto (lambda (ty bt bq acc)
	(begin
		(define pq p:sq)
		(define pn (p:value))
		(p:advance)
		(set! p:pending (p:cons (p:function ty pn 0 pq) p:pending))
		(if (= (p:kind) 'Comma) (p:advance) nil)
		(next p:localline/2 bt bq acc))))
(define p:localline/3 (lambda (ty0 bt bq acc)
	(begin
		(define fp (if (p:fnptr?) (p:fnptrdecl) false))
		(define ty (if fp p:fnty ty0))
		(define q (if fp 0 p:sq))
		(if fp (if (head fp) nil (p:die "bad local declaration"))
			(if (= (p:kind) 'Id) nil (p:die "bad local declaration")))
		(define n (if fp (head fp) (p:value)))
		(if fp nil (p:advance))
		(define size (if fp (p:second fp) (p:arraysize/t ty)))
		(define elem (if (= size nil) ty (p:elemty ty)))
		(define ty2 (if (= size nil) ty (+ elem p:PTR)))
		(define q2 (if (= size nil) q (bit:shl q 2)))
		;; scalars take a full (possibly non-constant) initializer
		;; expression; a brace list for an array or a struct takes
		;; expressions too (M7b, p:linit); a string keeps its form
		(define init
			(if (= (p:kind) 'Assign)
				(begin
					(p:advance)
					(if (if (= (p:kind) 'Lbrace) (if (= size nil) (p:svalue? ty) true) false)
						(p:linit)
					(if (if (= (p:kind) 'Lbrace) (= size nil) false)
						(p:scalarbraces)
					(if (= size nil)
						(list 'einit (p:expr 1))
					(p:init)))))
			nil))		(define fsize (p:initsize size elem init "local"))
		(set! p:locals (p:cons n p:locals))
		(set! p:lvars (p:cons (list n ty2 fsize) p:lvars))
		(if (= (p:kind) 'Comma) (p:advance) nil)
		(next p:localline/2 bt bq
			(p:cons (p:qnode (list 'local ty2 n fsize init) q2) acc)))))

;; a scalar local's initializer in braces, `= { E }` (C allows them, and
;; a trailing comma): the expression alone
(define p:scalarbraces (lambda ()
	(begin
		(p:advance)
		(define e (p:expr 1))
		(if (= (p:kind) 'Comma) (p:advance) nil)
		(p:expect 'Rbrace "} after a scalar's initializer")
		(list 'einit e))))
;; ... and a scalar global's: the one item of its brace list
(define p:unbrace (lambda (init)
	(if (= init nil) nil
	(if (= (head init) 'braces)
		(if (= (length (p:second init)) 1) (list 'num (head (p:second init)))
			(p:die "too many initializers for a scalar"))
	(if (= (head init) 'agg)
		(if (= (length (p:second init)) 1) (head (p:second init))
			(p:die "too many initializers for a scalar"))
	init)))))

;; all locals at the top of a function body (declarations can also
;; appear anywhere in a block; those become declstmt nodes). A typedef
;; there names its type for the rest of the function (p:function puts
;; the file's typedefs back after the body).
(define p:localdecls (lambda (acc)
	(if (= (p:kind) 'Typedef)
		(begin (p:typedefdecl) (next p:localdecls acc))
	(if (if (p:declstart?) true (if (= (p:kind) 'Static) true (= (p:kind) 'Extern)))
		(next p:localdecls (+ acc (p:localline)))
	(p:cons 'locals acc)))))

;; struct VALUE type: ty >= 1024 with pointer level 0
(define p:svalue? (lambda (ty)
	(if (< ty p:STRUCT0) false
	(if (p:rowty? ty) false
	(if (if (>= ty p:FLOAT) (< ty (+ p:DOUBLE 64)) false) false
	(= 0 (- (- ty p:STRUCT0)
		(* p:SSTEP (/ (- ty p:STRUCT0) p:SSTEP)))))))))

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
		(if (p:fnptr?)
			(begin
				(define fp (p:fnptrdecl))
				(if (head fp) nil (p:die "bad global declaration"))
				(p:globalvar/2 p:fnty (head fp) attr 0 (p:second fp)))
		(begin
			(define a2
				(if (= (p:kind) 'Attribute) (+ attr (p:attribute)) attr))
			(if (= (p:kind) 'Id) nil (p:die "bad global declaration"))
			(define n (p:value))
			(p:advance)
			(if (= (p:kind) 'Lparen)
				(p:function ty n a2 q)
			(p:globalvar ty n a2 q)))))))

;; K&R parameter declarations (M13), between a definition's ) and its {:
;; int f(a, b) char *a; int b; { ... }, which Rise of the Triad's qsort
;; comparisons are. p:params took each name as an int; each declaration
;; gives the one it names its type, and one no declaration names stays
;; int, as C89 has it.
(define p:krdecls (lambda (ps)
	(if (if (= (p:kind) 'Lbrace) true (if (= (p:kind) 'Semi) true (= (p:kind) 'Comma))) ps
	(begin
		(define bt (p:basetype p:INT))
		(next p:krdecls (p:krnames ps bt))))))
(define p:krnames (lambda (ps bt)
	(begin
		(define ty0 (p:stars bt p:bq))
		(if (= (p:kind) 'Id) nil (p:die "bad parameter declaration"))
		(define n (p:value))
		(p:advance)
		;; T name[] is a pointer to T, as in a prototype's parameters
		(define asz (p:arraysize/t ty0))
		(define ty (if (= asz nil) ty0 (+ (p:elemty ty0) p:PTR)))
		(define ps2 (p:krset ps n ty (list)))
		(if (= (p:kind) 'Comma) (begin (p:advance) (next p:krnames ps2 bt))
		(begin (p:expect 'Semi "semicolon after a parameter declaration") ps2)))))
(define p:krset (lambda (ps n ty acc)
	(if (empty? ps) (p:die (+ "a declaration of no parameter: " n))
	(if (= (p:second (head ps)) n)
		(+ (p:reverse acc) (p:cons (list ty n) (tail ps)))
	(next p:krset (tail ps) n ty (p:cons (head ps) acc))))))
(define p:function (lambda (ty n attr rq)
	(begin
		(p:advance)
		(define pv (p:params (list)))
		(define ps (p:krdecls (head pv)))
		(define va (p:second pv))
		(define a2 (if va (+ attr 32) attr))
		(if (if (= (p:kind) 'Semi) true (= (p:kind) 'Comma))
			;; prototype: extern, parameter types only; the ';' (or the
			;; ',' before the next declarator, as in `void A(void),
			;; B(void);`) stays for the declarator loop, as in c4cc, so
			;; "int f(), g;" keeps working. The masks, when there are
			;; any, follow.
			(begin
				(define pq (p:pquals ps (list)))
				(define pn (list 'proto ty n (p:ptypes ps (list)) va (+ a2 16)))
				(if (if (= rq 0) (p:allzero? pq) false) pn
					(+ pn (list rq pq))))
			(begin
				(p:expect 'Lbrace "function body")				(set! p:locals (p:paramnames ps (list)))
				(set! p:lvars (list))
				(set! p:curfn n)
				(set! p:srenames (list))
				(set! p:hiddens (list))
				(set! p:infn true)
				(define tds p:typedefs)
				(define ls (p:localdecls (list)))
				(define body (p:block/2 (list)))
				(set! p:infn false)
				(set! p:typedefs tds)
				(set! p:srenames (list))
				(set! p:locals (list))
				(set! p:lvars (list))
				;; compound literals' hidden locals go with the others, and
				;; keep the function from the inliner (attribute 128)
				(define ls2 (if (empty? p:hiddens) ls (+ ls (p:reverse p:hiddens))))
				(define a3 (if (empty? p:hiddens) a2 (bit:or a2 128)))
				(set! p:hiddens (list))
			(p:qnode (list 'func ty n ps va a3 ls2 body) rq))))))
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
		(define size (p:arraysize/t ty))
		(p:globalvar/2 (if (= size nil) ty (p:elemty ty)) n attr q size))))
;; SIZE already read: a function pointer's declarator holds its own
(define p:globalvar/2 (lambda (ty n attr q size)
	(begin
		(define elem ty)
		(define ty2 (if (= size nil) ty (+ ty p:PTR)))
		(define a2 (if (= size nil) attr (+ attr 64)))
		;; the global its own initializer may name: &S_sfx[sfx_pistol]
		(set! p:curgv (list n ty2 size))
		(define init0
			(if (= (p:kind) 'Assign)
				(begin
					(p:advance)
					(if (if (= size nil) (if (p:svalue? ty) false (p:dyninit?)) false)
						(begin
							(set! p:dyninits (p:cons (list n (p:expr 1) ty) p:dyninits))
							nil)
					(p:init)))
			nil))
		(set! p:curgv false)
		;; a scalar's initializer may be in braces
		(define init (if (if (= size nil) (not (p:svalue? ty)) false) (p:unbrace init0) init0))
		;; `extern T x[];` (M7b): a declaration, sized where x is defined
		(define fsize (if (if (= init nil) (if (= size 0) (= 16 (bit:and attr 16)) false) false)
			0 (p:initsize size elem init "global")))
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
		(set! p:gvars (p:cons (list n ty2 fsize) p:gvars))
		(p:qnode (list 'global ty2 n fsize a2 init)
			(if (= size nil) q (bit:shl q 2))))))

;; ---- initializers computed at start-up (M9) ----
;;
;; C lets a global's initializer be any constant expression, which gcc
;; folds -- floating point, and an address plus an offset. c4lc folds
;; neither: a float constant is its bits, and the address of another
;; unit's global is a patch that carries no offset. So a scalar global
;; whose initializer is not a form p:init takes is set by a constructor
;; instead: `const float radtoint = (float)(FINEANGLES/2/PI);` and
;; `fixed *costable = sintable+(ANGLES/4);` are stored before main runs,
;; by __c4lc_init, a static function the parser writes after the unit.
;; Nothing can tell, short of another constructor reading the global first.
(define p:dyninits nil)   ;; ((NAME EXPR TYPE) ...), the last first
;; is the initializer at hand one that p:init cannot take? Not when it is
;; a string, &NAME alone, a NAME alone, a float literal alone, or a constant
;; expression -- one with no string, float, &, type name or name that is
;; no constant up to the next , or ; (a sizeof's operand aside: c4lc-
;; parse's p:constsizeof answers sizeof of a global)
(define p:endtok? (lambda (k) (if (= k 'Comma) true (= k 'Semi))))
(define p:dyninit? (lambda ()
	(begin
		(define k (p:kind))
		(define k2 (head (head (tail p:toks))))
		(if (= k 'Str) false
		;; &a[k] is a + k, set at start-up (Doom's `boolean *mousebuttons =
		;; &mousearray[1];`)
		(if (= k 'And) (if (= k2 'Id) (if (p:endtok? (head (head (tail (tail p:toks))))) false true) true)
		(if (= k 'Lbrace) false
		(if (if (= k 'Id) (p:endtok? k2) false) false
		(if (if (p:fptok? k) (p:endtok? k2) false) false
		(if (if (= k 'Sub) (if (p:fptok? k2) (p:endtok? (head (head (tail (tail p:toks))))) false) false) false
		(p:dynscan p:toks 0))))))))))
(define p:dynscan (lambda (ts d)
	(begin
		(define t (head ts))
		(define k (head t))
		(if (= k 'Eof) false
		(if (if (= d 0) (p:endtok? k) false) false
		(if (= k 'Sizeof)
			(next p:dynscan
				(if (= (head (head (tail ts))) 'Lparen) (p:skipgroup (tail ts) 0)
				(p:skipunary (tail ts)))
				d)
		(if (if (= k 'Str) true (if (p:fptok? k) true (= k 'And))) true
		(if (p:declstart-tok? t) true
		(if (if (= k 'Id) (= (p:constname? (p:second t)) false) false) true
		(next p:dynscan (tail ts)
			(if (if (= k 'Lparen) true (= k 'Brak)) (+ d 1)
			(if (if (= k 'Rparen) true (= k 'Rbrak)) (- d 1) d))))))))))))
;; the tokens after `*p`, `name`, `a[i].m` -- sizeof's operand unparenthesized
(define p:skipunary (lambda (ts)
	(begin
		(define k (head (head ts)))
		(if (if (= k 'Mul) true (= k 'And)) (next p:skipunary (tail ts))
		(if (= k 'Id) (p:skippost (tail ts))
		ts)))))
(define p:skippost (lambda (ts)
	(begin
		(define k (head (head ts)))
		(if (= k 'Brak) (next p:skippost (p:skipbrak (tail ts) 1))
		(if (if (= k 'Dot) true (= k 'Arrow)) (next p:skippost (tail (tail ts)))
		ts)))))
(define p:skipbrak (lambda (ts d)
	(begin
		(define k (head (head ts)))
		(if (= k 'Eof) ts
		(if (if (= k 'Rbrak) (= d 1) false) (tail ts)
		(next p:skipbrak (tail ts) (if (= k 'Brak) (+ d 1) (if (= k 'Rbrak) (- d 1) d))))))))
;; the tokens after a parenthesized group: ts starts at its (
(define p:skipgroup (lambda (ts d)
	(begin
		(define k (head (head ts)))
		(define d2 (if (= k 'Lparen) (+ d 1) (if (= k 'Rparen) (- d 1) d)))
		(if (= k 'Eof) ts
		(if (if (= k 'Rparen) (= d2 0) false) (tail ts)
		(next p:skipgroup (tail ts) d2))))))
;; It goes after the unit and before the long long and float helpers
;; appended to it (p:withll, p:withfp): c4lc-gen compiles a helper only
;; when something before it has called it.
(define p:insdyn (lambda (ds acc fn)
	(if (empty? ds) (+ (p:reverse acc) fn)
	(if (p:helper? (head ds)) (+ (+ (p:reverse acc) fn) ds)
	(next p:insdyn (tail ds) (p:cons (head ds) acc) fn)))))
(define p:hpre? (lambda (n pre)
	(if (> (length n) (length pre)) (= (string:substr n 0 (length pre)) pre) false)))
(define p:helper? (lambda (d)
	(if (= (head d) 'func)
		(if (p:hpre? (index d 2) "__c4ll_") true
		(if (p:hpre? (index d 2) "__c4f_") true
		(if (p:hpre? (index d 2) "__c4d_") true
		(p:hpre? (index d 2) "__c4fp_"))))
	false)))
;; the constructor, when the unit needs one
(define p:dynfunc (lambda ()
	(if (empty? p:dyninits) (list)
	(list (list 'func p:INT "__c4lc_init" (list) false 9 (list 'locals)
		(p:cons 'block (p:dynstmts (p:reverse p:dyninits) (list))))))))
(define p:dynstmts (lambda (l acc)
	(if (empty? l) (p:reverse acc)
	(next p:dynstmts (tail l)
		;; *(T *)&NAME = EXPR: a const global is still set, once
		(p:cons (list 'expr (list 'assign
			(list 'deref (list 'cast (+ (index (head l) 2) p:PTR) (list 'addr (list 'var (head (head l))))))
			(p:second (head l)))) acc)))))

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
		(define ty0 (p:stars bt p:bq))
		(define fp (if (p:fnptr?) (p:fnptrdecl) false))
		(define ty (if fp p:fnty ty0))
		(define q (if fp 0 p:sq))
		(if fp (if (head fp) nil (p:die "typedef needs a name"))
			(if (= (p:kind) 'Id) nil (p:die "typedef needs a name")))
		(define n (if fp (head fp) (p:value)))
		(if fp nil (p:advance))
		;; typedef T NAME[N] (ports-feasibility §8 step 6): an array type
		;; is a row (M9), N elements of T, which stands for the address of
		;; its first element wherever its value is used, as an array does.
		;; sha1.h's `typedef byte sha1_digest_t[20];`
		(define asz (if fp nil (p:arraysize)))
		(if (= asz 0) (p:die "typedef of an array needs its size") nil)
		(define ty2 (if (= asz nil) ty (p:rowtype (p:elemty ty) asz)))
		(define q2 (if (= asz nil) q 0))
		(p:expect 'Semi "semicolon after typedef")
		(set! p:typedefs (p:cons (list n ty2 q2) p:typedefs))
		(p:qnode (list 'typedefd n ty2) q2))))

(define p:top (lambda (acc)
	(if (= (p:kind) 'Eof) (p:insdyn (p:reverse (p:drain acc)) (list) (p:dynfunc))
	(if (= (p:kind) 'PragmaPack) (begin (p:pragmapack) (next p:top acc))
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
			(next p:top (p:hoist (p:declarators bt p:bq attr (p:drain acc))))))))))))

;; ---- what c4 has no use for (M7b) ----
;;
;; Before parsing, the tokens lose `inline`, `restrict`, `__extension__`
;; and `register` in their spellings, and every __attribute__((...)) that names neither
;; constructor nor destructor -- noreturn, unused, format, aligned,
;; regparm and the rest of what Busybox's platform.h writes on nearly
;; every prototype. They change nothing c4 does. The two that do stay
;; where p:attribute reads them. c4fc's UNTAG is the same filter.
;; `register` alone before a declarator's name, Doom's `register
;; outcode1 = 0;`, is the int C89 made of it.
(define p:dropids '("inline" "__inline" "__inline__" "restrict" "__restrict"
	"__restrict__" "__extension__" "register"))
(define p:untag (lambda (toks acc)
	(if (empty? toks) (p:reverse acc)
	(begin
		(define t (head toks))
		(if (if (= (head t) 'Id) (p:member? (p:second t) p:dropids) false)
			(if (p:implicitint? t (tail toks))
				(next p:untag (tail toks) (p:cons (+ (list 'Int 0) (tail (tail t))) acc))
			(next p:untag (tail toks) acc))
		(if (= (head t) 'Attribute)
			(begin
				(define g (p:attrgroup (tail toks) 0 false))
				(if (p:second g) (next p:untag (tail toks) (p:cons t acc))
				(next p:untag (head g) acc)))
		(next p:untag (tail toks) (p:cons t acc))))))))
;; `register NAME` and then = , ; [ or ): NAME's type is int
(define p:implicitint? (lambda (t rest)
	(if (= (p:second t) "register")
		(if (if (empty? rest) false (if (= (head (head rest)) 'Id) (not (empty? (tail rest))) false))
			(p:member?/atom (head (head (tail rest))) '(Assign Comma Semi Brak Rparen))
		false)
	false)))
(define p:member?/atom (lambda (x l)
	(if (empty? l) false (if (= (head l) x) true (next p:member?/atom x (tail l))))))
;; from the '(' after __attribute__ to its matching ')': (REST KEEP)
(define p:attrgroup (lambda (toks d keep)
	(if (empty? toks) (list toks keep)
	(begin
		(define k (head (head toks)))
		(define d2 (if (= k 'Lparen) (+ d 1) (if (= k 'Rparen) (- d 1) d)))
		(define keep2 (if keep true (if (= k 'Constructor) true (= k 'Destructor))))
		(if (if (= d2 0) (= k 'Rparen) false) (list (tail toks) keep2)
		(next p:attrgroup (tail toks) d2 keep2))))))

;; The long long helpers (M8, c4lc-gen's g:llbinary and friends), for a
;; unit on a 32-bit target that says `long long`: the parser reads them
;; after the unit, and the generator compiles only the ones it called.
;; A caller comes before what it calls, so a helper's own calls are seen
;; before its turn.
(define p:llsrc "
static int *__c4ll_addr(int *d, int *a, int *b) {
    return __c4ll_add(a, a, d);
}
static int *__c4ll_add(int *d, int *a, int *b) {
    unsigned lo; int hi;
    lo = (unsigned)a[0] + (unsigned)b[0];
    hi = a[1] + b[1] + (lo < (unsigned)a[0]);
    d[0] = lo; d[1] = hi;
    return d;
}
static int *__c4ll_sub(int *d, int *a, int *b) {
    unsigned lo; int hi;
    lo = (unsigned)a[0] - (unsigned)b[0];
    hi = a[1] - b[1] - ((unsigned)a[0] < (unsigned)b[0]);
    d[0] = lo; d[1] = hi;
    return d;
}
static int *__c4ll_mul(int *d, int *a, int *b) {
    unsigned al, ah, bl, bh, p0, p1, p2, t, lo, hi;
    al = (unsigned)a[0] & 65535; ah = (unsigned)a[0] >> 16;
    bl = (unsigned)b[0] & 65535; bh = (unsigned)b[0] >> 16;
    p0 = al * bl; p1 = al * bh; p2 = ah * bl;
    hi = ah * bh + (p1 >> 16) + (p2 >> 16);
    t = p0 + (p1 << 16); hi = hi + (t < p0);
    lo = t + (p2 << 16); hi = hi + (lo < t);
    hi = hi + (unsigned)a[0] * (unsigned)b[1] + (unsigned)a[1] * (unsigned)b[0];
    d[0] = lo; d[1] = hi;
    return d;
}
static int *__c4ll_and(int *d, int *a, int *b) { d[0] = a[0] & b[0]; d[1] = a[1] & b[1]; return d; }
static int *__c4ll_or(int *d, int *a, int *b) { d[0] = a[0] | b[0]; d[1] = a[1] | b[1]; return d; }
static int *__c4ll_xor(int *d, int *a, int *b) { d[0] = a[0] ^ b[0]; d[1] = a[1] ^ b[1]; return d; }
static int *__c4ll_shl(int *a, int n) {
    unsigned lo, hi;
    n = n & 63; lo = a[0]; hi = a[1];
    if (n >= 32) { hi = lo << (n - 32); lo = 0; }
    else if (n > 0) { hi = (hi << n) | (lo >> (32 - n)); lo = lo << n; }
    a[0] = lo; a[1] = hi;
    return a;
}
static int *__c4ll_shr(int *a, int n) {
    unsigned lo, hi;
    n = n & 63; lo = a[0]; hi = a[1];
    if (n >= 32) { lo = hi >> (n - 32); hi = 0; }
    else if (n > 0) { lo = (lo >> n) | (hi << (32 - n)); hi = hi >> n; }
    a[0] = lo; a[1] = hi;
    return a;
}
static int *__c4ll_sar(int *a, int n) {
    unsigned lo; int hi;
    n = n & 63; lo = a[0]; hi = a[1];
    if (n >= 32) { lo = hi >> (n - 32); hi = hi >> 31; }
    else if (n > 0) { lo = (lo >> n) | ((unsigned)hi << (32 - n)); hi = hi >> n; }
    a[0] = lo; a[1] = hi;
    return a;
}
static int __c4ll_cmp(int *a, int *b) {
    if (a[1] != b[1]) return a[1] < b[1] ? -1 : 1;
    if (a[0] == b[0]) return 0;
    return (unsigned)a[0] < (unsigned)b[0] ? -1 : 1;
}
static int __c4ll_ucmp(int *a, int *b) {
    if (a[1] != b[1]) return (unsigned)a[1] < (unsigned)b[1] ? -1 : 1;
    if (a[0] == b[0]) return 0;
    return (unsigned)a[0] < (unsigned)b[0] ? -1 : 1;
}
static int __c4ll_nz(int *a) { return (a[0] | a[1]) != 0; }
static int *__c4ll_div(int *d, int *a, int *b) {
    int x[2], y[2], r[2], neg;
    x[0] = a[0]; x[1] = a[1]; y[0] = b[0]; y[1] = b[1]; neg = 0;
    if (x[1] < 0) { __c4ll_neg(x, x); neg = 1; }
    if (y[1] < 0) { __c4ll_neg(y, y); neg = !neg; }
    __c4ll_udivmod(d, r, x, y);
    if (neg) __c4ll_neg(d, d);
    return d;
}
static int *__c4ll_mod(int *d, int *a, int *b) {
    int x[2], y[2], q[2], neg;
    x[0] = a[0]; x[1] = a[1]; y[0] = b[0]; y[1] = b[1]; neg = x[1] < 0;
    if (neg) __c4ll_neg(x, x);
    if (y[1] < 0) __c4ll_neg(y, y);
    __c4ll_udivmod(q, d, x, y);
    if (neg) __c4ll_neg(d, d);
    return d;
}
static int *__c4ll_udiv(int *d, int *a, int *b) { int r[2]; __c4ll_udivmod(d, r, a, b); return d; }
static int *__c4ll_umod(int *d, int *a, int *b) { int q[2]; __c4ll_udivmod(q, d, a, b); return d; }
static int __c4ll_udivmod(int *q, int *r, int *a, int *b) {
    unsigned nl, nh, bl, bh, rl, rh, ql, qh, t;
    int i;
    nl = a[0]; nh = a[1]; bl = b[0]; bh = b[1];
    if (nh == 0 && bh == 0) {
        q[0] = nl / bl; q[1] = 0; r[0] = nl % bl; r[1] = 0;
        return 0;
    }
    rl = 0; rh = 0; ql = 0; qh = 0; i = 0;
    while (i < 64) {
        rh = (rh << 1) | (rl >> 31);
        rl = (rl << 1) | (nh >> 31);
        nh = (nh << 1) | (nl >> 31);
        nl = nl << 1;
        qh = (qh << 1) | (ql >> 31);
        ql = ql << 1;
        if (rh > bh || (rh == bh && rl >= bl)) {
            t = rl; rl = rl - bl; rh = rh - bh - (t < bl);
            ql = ql | 1;
        }
        i = i + 1;
    }
    q[0] = ql; q[1] = qh; r[0] = rl; r[1] = rh;
    return 0;
}
static int *__c4ll_neg(int *d, int *a) {
    int lo, hi;
    lo = 0 - a[0];
    hi = ~a[1] + (a[0] == 0);
    d[0] = lo; d[1] = hi;
    return d;
}
")
;; float and double literals (docs/c4-float.md): the lexer has their
;; bits. A float is (num BITS FLOAT); a double (num BITS DOUBLE) on a
;; 64-bit target and (llnum LO HI DOUBLE) on a 32-bit one, where it is
;; carried as long long is. NEG flips the sign bit: -1.5 folds as -1
;; does, so a global can be initialized with it.
(define p:fptok? (lambda (k) (if (= k 'FNumF) true (= k 'FNumD))))
(define p:fpnum? (lambda (n)
	(if (= (length n) 3) (if (= (index n 2) p:FLOAT) true (= (index n 2) p:DOUBLE)) false)))
(define p:fplit (lambda (neg)
	(begin
		(define k (p:kind))
		(define v (p:value))
		(p:advance)
		(if (= k 'FNumF) (list 'num (if neg (bit:xor v p:SIGN32) v) p:FLOAT)
		(begin
			(define lo (head v))
			(define hi (if neg (bit:xor (p:second v) p:SIGN32) (p:second v)))
			(if (= p:WORD 8) (list 'num (bit:or (bit:shl hi 32) (bit:and lo p:M32)) p:DOUBLE)
			(list 'llnum lo hi p:DOUBLE)))))))

;; ---- constant initializers, folded (docs/ports-feasibility.md §8, 4-5) ----
;;
;; An element of a global's brace list that p:init's own forms do not
;; cover -- a cast, an address plus an offset (`&S_sfx[sfx_pistol]`, a
;; row `seq[0]`, `(int *)&x`, `(int)"str"`), or floating point (`{ -.867
;; * R, -.5 * R }`) -- is parsed as an expression and folded here, as gcc
;; folds it:
;;   - integers by p:tfold, as p:const folds them;
;;   - floating point exactly, as a rational over lex's big integers,
;;     rounded to float or double after every operation as C rounds;
;;   - an address as a global's name and a byte offset, which becomes a
;;     data patch that carries the offset (c4lc-gen's g:addrpatchk).
;; A value is
;;   (int V T)              an integer of type T
;;   (flt SIGN NUM DEN T)   (-1)^SIGN * NUM/DEN, a float or a double
;;   (adr NAME OFF PT)      NAME's address plus OFF bytes, of type PT
;;   (sad STR OFF PT)       a string literal's, likewise
;; and p:foldinit makes it an initializer: (num V), (fval FLT), (name N),
;; (gaddr N OFF), (str S) or (sadr S OFF). c4lc-gen converts an fval to
;; the type of what it initializes (g:fpinit).
(define p:curgv false)    ;; (NAME TYPE SIZE) of the global being initialized
(define p:ilend? (lambda (k) (if (= k 'Comma) true (if (= k 'Rbrace) true (= k 'Semi)))))
(define p:foldleaf? (lambda ()
	(begin
		(define k (p:kind))
		(define k2 (head (head (tail p:toks))))
		(define k3 (if (p:ilend? k2) 'Comma (head (head (tail (tail p:toks))))))
		(if (= k 'Lbrace) false
		;; a string, or strings p:strlit joins; "abc" + 1 is folded
		(if (if (= k 'Str) (if (= k2 'Str) true (p:ilend? k2)) false) false
		(if (if (= k 'Id) (p:ilend? k2) false) false
		(if (if (p:fptok? k) (p:ilend? k2) false) false
		(if (if (= k 'Sub) (if (p:fptok? k2) (p:ilend? k3) false) false) false
		(if (if (= k 'And) (if (= k2 'Id) (p:ilend? k3) false) false) false
		(if (= k 'Str) true
		(p:foldscan p:toks 0)))))))))))
;; up to the element's end: a string, a float, an &, a type name or a name
;; that is no constant (a sizeof's operand aside, as in p:dynscan)
(define p:foldscan (lambda (ts d)
	(begin
		(define t (head ts))
		(define k (head t))
		(if (= k 'Eof) false
		(if (if (= d 0) (p:ilend? k) false) false
		(if (= k 'Sizeof)
			(next p:foldscan
				(if (= (head (head (tail ts))) 'Lparen) (p:skipgroup (tail ts) 0)
				(p:skipunary (tail ts)))
				d)
		(if (if (= k 'Str) true (if (p:fptok? k) true (= k 'And))) true
		(if (p:declstart-tok? t) true
		(if (if (= k 'Id) (= (p:constname? (p:second t)) false) false) true
		(next p:foldscan (tail ts)
			(if (if (= k 'Lparen) true (= k 'Brak)) (+ d 1)
			(if (if (= k 'Rparen) true (= k 'Rbrak)) (- d 1) d))))))))))))
(define p:foldinit (lambda ()
	(begin
		(define v (p:cfold (p:expr 1)))
		(define h (head v))
		(if (= h 'int) (list 'num (p:second v))
		(if (= h 'flt) (list 'fval v)
		(if (= h 'adr)
			(if (= (p:third v) 0) (list 'name (p:second v)) (list 'gaddr (p:second v) (p:third v)))
		(if (= (p:third v) 0) (list 'str (p:second v)) (list 'sadr (p:second v) (p:third v)))))))))

(define p:cfdie (lambda () (p:die "initializer element is not a constant")))
(define p:cfold (lambda (e)
	(begin
		(define h (head e))
		(if (= h 'num) (p:cfnum e)
		(if (= h 'llnum) (p:cfllnum e)
		(if (= h 'str) (list 'sad (p:second e) 0 p:PTR)
		(if (= h 'var) (p:cfvar (p:second e))
		(if (= h 'addr) (p:cflv (p:second e))
		(if (if (= h 'index) true (if (= h 'member) true (= h 'arrow))) (p:cfdecay e)
		(if (= h 'cast) (p:cfcast (p:second e) (p:cfold (p:third e)))
		(if (= h 'neg) (p:cfneg (p:cfold (p:second e)))
		(if (= h 'cond)
			(if (p:cftrue? (p:cfold (p:second e))) (p:cfold (p:third e)) (p:cfold (index e 3)))
		(if (= h 'lognot) (list 'int (if (p:cftrue? (p:cfold (p:second e))) 0 1) p:INT)
		(if (= h 'bitnot) (p:cfbin 'bxor (list 'int -1 p:INT) (p:cfold (p:second e)))
		(if (= h 'land)
			(list 'int (if (p:cftrue? (p:cfold (p:second e))) (if (p:cftrue? (p:cfold (p:third e))) 1 0) 0) p:INT)
		(if (= h 'lor)
			(list 'int (if (p:cftrue? (p:cfold (p:second e))) 1 (if (p:cftrue? (p:cfold (p:third e))) 1 0)) p:INT)
		(if (= h 'sizeof) (list 'int (p:ctysize (p:second e)) p:INT)
		(if (= h 'sizeofa) (list 'int (p:csizeofx (list 'var (p:second e)) "initializer") p:INT)
		(if (= h 'sizeofe) (list 'int (p:csizeofx (p:second e) "initializer") p:INT)
		(if (p:assoc/atom h p:cfops) (p:cfbin h (p:cfold (p:second e)) (p:cfold (p:third e)))
		(p:cfdie)))))))))))))))))))))
(define p:cfops '((add) (sub) (mul) (div) (mod) (shl) (shr) (band) (bor) (bxor)
	(eq) (ne) (lt) (gt) (le) (ge)))
;; a literal: an integer, or a float's or a double's bits
(define p:cfnum (lambda (e)
	(begin
		(define t (if (= (length e) 3) (index e 2) p:INT))
		(if (= t p:FLOAT) (p:w32rat (p:second e))
		(if (= t p:DOUBLE)
			(p:w64rat (bit:and (p:second e) p:M32) (bit:and (bit:shr (p:second e) 32) p:M32))
		(list 'int (p:second e) t))))))
(define p:cfllnum (lambda (e)
	(if (= (index e 3) p:DOUBLE) (p:w64rat (p:second e) (p:third e))
	;; a long long constant on a 32-bit target: only one a word holds
	(if (= (p:third e) (if (< (p:second e) 0) -1 0)) (list 'int (p:second e) p:INT)
	(p:die "a long long constant is too wide for an initializer here")))))
;; a global, by name: the one being initialized, or one declared before
(define p:cfgv (lambda (n)
	(if (if p:curgv (= (head p:curgv) n) false) p:curgv
	(if (p:member? n p:locals) false
	(p:assoc n p:gvars)))))
;; a name's value: a static const's, or an array's address. A name that
;; is no global's is taken for a function, which c4lc-gen checks.
(define p:cfvar (lambda (n)
	(begin
		(define sc (if (p:member? n p:locals) false (p:assoc n p:sconsts)))
		(define g (if sc false (p:cfgv n)))
		(if sc (list 'int (p:second sc) (index sc 2))
		(if (= g false) (list 'adr n 0 p:INT)
		(if (= (index g 2) nil) (p:cfdie)
		(list 'adr n 0 (p:second g))))))))
;; the address of an lvalue
(define p:cflv (lambda (x)
	(begin
		(define h (head x))
		(if (= h 'var)
			(begin
				(define g (p:cfgv (p:second x)))
				(if (= g false) (list 'adr (p:second x) 0 p:INT)
				(list 'adr (p:second x) 0 (if (= (index g 2) nil) (+ (p:second g) p:PTR) (p:second g)))))
		(if (= h 'index) (p:cfbin 'add (p:cfold (p:second x)) (p:cfold (p:third x)))
		(if (if (= h 'member) true (= h 'arrow))
			(begin
				(define b (if (= h 'member) (p:cflv (p:second x)) (p:cfold (p:second x))))
				(if (if (= (head b) 'adr) (p:ptrty? (index b 3)) false) nil (p:cfdie))
				(define r (p:smember (- (index b 3) p:PTR) (p:third x)))
				(list 'adr (p:second b) (+ (p:third b) (p:second r))
					(if (p:arrmem? r) (p:third r) (+ (p:third r) p:PTR))))
		(if (= h 'deref) (p:cfold (p:second x))
		(if (= h 'str) (list 'sad (p:second x) 0 p:PTR)
		(p:cfdie)))))))))
;; an array member's record: its type is its element's pointer, and it
;; has bytes of its own (a struct member has them too)
(define p:arrmem? (lambda (r)
	(if (= (index r 3) nil) false (p:ptrty? (p:third r)))))
;; an element or a member used as a value: only an array decays to a
;; constant, its address -- a row of an array of arrays, or an array member
(define p:cfdecay (lambda (e)
	(begin
		(define a (p:cflv e))
		(if (= (head a) 'adr) nil (p:cfdie))
		(if (= (head e) 'index)
			(begin
				(define row (p:assoc (- (index a 3) p:PTR) p:rows))
				(if row (list 'adr (p:second a) (p:third a) (+ (p:second row) p:PTR)) (p:cfdie)))
		(begin
			(define b (if (= (head e) 'member) (p:cflv (p:second e)) (p:cfold (p:second e))))
			(if (p:arrmem? (p:smember (- (index b 3) p:PTR) (p:third e))) a (p:cfdie)))))))
(define p:ptrty? (lambda (t)
	(if (< t 512) (>= t 2) (> (- (- t 512) (* 64 (/ (- t 512) 64))) 0))))
(define p:fpty? (lambda (t) (if (= t p:FLOAT) true (= t p:DOUBLE))))
(define p:cftrue? (lambda (v)
	(if (= (head v) 'int) (!= (p:second v) 0)
	(if (= (head v) 'flt) (> (lex:bbitlen (index v 2)) 0)
	true))))
;; (T) v
(define p:cfcast (lambda (t v)
	(begin
		(define h (head v))
		(if (p:fpty? t)
			(if (= h 'int) (p:ratround (p:int2rat (p:second v) (index v 2)) t)
			(if (= h 'flt) (p:ratround v t)
			(p:cfdie)))
		(if (p:svalue? t) (p:cfdie)
		(if (if (= h 'adr) true (= h 'sad)) (list h (p:second v) (p:third v) t)
		(if (= h 'flt)
			(if (p:ptrty? t) (p:cfdie) (list 'int (p:convval (p:rat2int v) t) t))
		(list 'int (if (p:intty? t) (p:convval (p:second v) t) (p:second v)) t))))))))
(define p:cfneg (lambda (v)
	(if (= (head v) 'flt) (list 'flt (- 1 (p:second v)) (index v 2) (index v 3) (index v 4))
	(p:cfbin 'sub (list 'int 0 p:INT) v))))
;; how far apart the things a pointer of type pt points at are: an integer
;; holding an address (a cast made it one) counts bytes
(define p:cfstride (lambda (pt)
	(if (p:ptrty? pt) (p:estride (- pt p:PTR)) 1)))
(define p:cfbin (lambda (op a b)
	(begin
		(define ha (head a))
		(define hb (head b))
		(define aa (if (= ha 'adr) true (= ha 'sad)))
		(define ba (if (= hb 'adr) true (= hb 'sad)))
		(if (if aa (= hb 'int) false)
			(if (= op 'add) (list ha (p:second a) (+ (p:third a) (* (p:second b) (p:cfstride (index a 3)))) (index a 3))
			(if (= op 'sub) (list ha (p:second a) (- (p:third a) (* (p:second b) (p:cfstride (index a 3)))) (index a 3))
			(p:cfdie)))
		(if (if ba (= ha 'int) false)
			(if (= op 'add) (p:cfbin 'add b a) (p:cfdie))
		(if (if aa true ba) (p:cfdie)
		(if (if (= ha 'flt) true (= hb 'flt)) (p:cfflt op a b)
		(begin
			(define v (p:tfold op (p:second a) (index a 2) (p:second b) (index b 2)))
			(if (= v 'nofold) (p:cfdie) nil)
			(list 'int v p:ft)))))))))
;; floating point: the usual arithmetic conversions, the exact operation,
;; then the rounding to the result's type
(define p:cfflt (lambda (op a b)
	(begin
		(define t (if (if (= (head a) 'flt) (= (index a 4) p:DOUBLE) false) p:DOUBLE
			(if (if (= (head b) 'flt) (= (index b 4) p:DOUBLE) false) p:DOUBLE p:FLOAT)))
		(define x (p:cfcast t a))
		(define y (p:cfcast t b))
		(if (= op 'add) (p:ratround (p:ratadd x y) t)
		(if (= op 'sub) (p:ratround (p:ratadd x (p:cfneg y)) t)
		(if (= op 'mul) (p:ratround (p:ratmul x y) t)
		(if (= op 'div) (p:ratround (p:ratdiv x y) t)
		(begin
			(define c (p:ratcmp x y))
			(define r
				(if (= op 'eq) (= c 0) (if (= op 'ne) (!= c 0) (if (= op 'lt) (< c 0)
				(if (= op 'gt) (> c 0) (if (= op 'le) (<= c 0) (if (= op 'ge) (>= c 0) (p:cfdie))))))))
			(list 'int (if r 1 0) p:INT)))))))))

;; ---- exact rationals over lex's big integers (16-bit limbs, lowest first) ----
(define p:bzero? (lambda (a) (= (lex:bbitlen a) 0)))
(define p:badd (lambda (a b c acc)
	(if (if (empty? a) (empty? b) false) (lex:brev (if (= c 0) acc (p:cons c acc)) (list))
	(begin
		(define x (+ (+ (if (empty? a) 0 (head a)) (if (empty? b) 0 (head b))) c))
		(next p:badd (if (empty? a) a (tail a)) (if (empty? b) b (tail b))
			(bit:shr x 16) (p:cons (bit:and x 65535) acc))))))
(define p:bmulb (lambda (a b i acc)
	(if (empty? b) acc
	(next p:bmulb a (tail b) (+ i 16)
		(p:badd acc (lex:bshl (lex:bmul a (head b) 0 (list)) i) 0 (list))))))
(define p:bmul (lambda (a b) (p:bmulb a b 0 (list 0))))
(define p:bpow2 (lambda (n) (lex:bshl (list 1) n)))
;; the quotient of a by b, both big, rounded toward zero
(define p:bdiv (lambda (a b)
	(next p:bdiv/2 a b (- (lex:bbitlen a) (lex:bbitlen b)) (list 0))))
(define p:bdiv/2 (lambda (a b i q)
	(if (< i 0) q
	(begin
		(define bi (lex:bshl b i))
		(if (>= (lex:bcmp a bi) 0)
			(next p:bdiv/2 (lex:bsub a bi 0 (list)) b (- i 1) (lex:bmul q 2 1 (list)))
		(next p:bdiv/2 a b (- i 1) (lex:bmul q 2 0 (list))))))))
;; a word's limbs, as the unsigned number its bits are
(define p:wlimbs (lambda (v i acc)
	(if (>= i p:BITS) (lex:brev acc (list))
	(next p:wlimbs v (+ i 16) (p:cons (bit:and (bit:shr v i) 65535) acc)))))
;; ... and back: the low word of a big number
(define p:limbsw (lambda (a i acc)
	(if (if (empty? a) true (>= i p:BITS)) acc
	(next p:limbsw (tail a) (+ i 16) (bit:or acc (bit:shl (head a) i))))))
(define p:unsigned? (lambda (t)
	(begin
		(define pt (p:promote t))
		(if (= pt p:UINT) true (if (= pt p:ULONG) true (= t p:ULL))))))
;; integer v of type t, exactly
(define p:int2rat (lambda (v t)
	(if (if (< v 0) (not (p:unsigned? t)) false)
		(list 'flt 1 (lex:bmul (p:wlimbs (bit:xor v -1) 0 (list)) 1 1 (list)) (list 1) p:DOUBLE)
	(list 'flt 0 (p:wlimbs v 0 (list)) (list 1) p:DOUBLE))))
;; toward zero, to the word
(define p:rat2int (lambda (r)
	(begin
		(define m (p:limbsw (p:bdiv (index r 2) (index r 3)) 0 0))
		(if (= (p:second r) 1) (- 0 m) m))))
(define p:ratmul (lambda (x y)
	(list 'flt (bit:xor (p:second x) (p:second y))
		(p:bmul (index x 2) (index y 2)) (p:bmul (index x 3) (index y 3)) (index x 4))))
(define p:ratdiv (lambda (x y)
	(begin
		(if (p:bzero? (index y 2)) (p:die "division by zero in a constant") nil)
		(list 'flt (bit:xor (p:second x) (p:second y))
			(p:bmul (index x 2) (index y 3)) (p:bmul (index x 3) (index y 2)) (index x 4)))))
(define p:ratadd (lambda (x y)
	(begin
		(define a (p:bmul (index x 2) (index y 3)))
		(define b (p:bmul (index y 2) (index x 3)))
		(define d (p:bmul (index x 3) (index y 3)))
		(if (= (p:second x) (p:second y)) (list 'flt (p:second x) (p:badd a b 0 (list)) d (index x 4))
		(begin
			(define c (lex:bcmp a b))
			(if (= c 0) (list 'flt 0 (list 0) (list 1) (index x 4))
			(if (> c 0) (list 'flt (p:second x) (lex:bsub a b 0 (list)) d (index x 4))
			(list 'flt (p:second y) (lex:bsub b a 0 (list)) d (index x 4)))))))))
;; the sign of x - y
(define p:ratcmp (lambda (x y)
	(begin
		(define r (p:ratadd x (p:cfneg y)))
		(if (p:bzero? (index r 2)) 0 (if (= (p:second r) 1) -1 1)))))
;; a format's significand bits and exponent range
(define p:fmt (lambda (t) (if (= t p:FLOAT) (list 24 -126 127) (list 53 -1022 1023))))
;; the rounding of r to type t: (BIASEDEXP FRACTION), lex:fpconv's answer
(define p:ratbits (lambda (r t)
	(begin
		(define f (p:fmt t))
		(define p (head f))
		(define a (index r 2))
		(define b (index r 3))
		(if (p:bzero? a) (list 0 (list 0))
		(begin
			(define e (lex:fpscale a b (- (+ p 1) (- (lex:bbitlen a) (lex:bbitlen b))) p (p:second f) (index f 2)))
			(if (= (head e) (+ (* 2 (index f 2)) 1))
				(p:die "a floating constant is out of range") nil)
			e)))))
;; r rounded to type t, still a rational: the bits' own value
(define p:ratround (lambda (r t)
	(begin
		(define f (p:fmt t))
		(define p (head f))
		(define e (p:ratbits r t))
		(define be (head e))
		(define m (if (= be 0) (p:second e) (p:badd (p:second e) (p:bpow2 (- p 1)) 0 (list))))
		(define x (if (= be 0) (- (+ (p:second f) 1) p) (- (+ (- be (index f 2)) 1) p)))
		(if (>= x 0) (list 'flt (p:second r) (lex:bshl m x) (list 1) t)
		(list 'flt (p:second r) m (p:bpow2 (- 0 x)) t)))))
;; the bits of r rounded to a float: a word; to a double: (LO HI)
(define p:rat32 (lambda (r)
	(begin
		(define e (p:ratbits r p:FLOAT))
		(bit:or (if (= (p:second r) 1) p:SIGN32 0)
			(bit:or (bit:shl (head e) 23) (lex:bword (p:second e) 0))))))
(define p:rat64 (lambda (r)
	(begin
		(define e (p:ratbits r p:DOUBLE))
		(list (lex:bword (p:second e) 0)
			(bit:or (if (= (p:second r) 1) p:SIGN32 0)
				(bit:or (bit:shl (head e) 20) (lex:bword (p:second e) 1)))))))
;; ... and a float's or a double's bits as a rational
(define p:pow2rat (lambda (s m x t)
	(if (>= x 0) (list 'flt s (lex:bshl m x) (list 1) t)
	(list 'flt s m (p:bpow2 (- 0 x)) t))))
(define p:w32rat (lambda (w)
	(begin
		(define s (if (= 0 (bit:and w p:SIGN32)) 0 1))
		(define be (bit:and (bit:shr w 23) 255))
		(define fr (bit:and w 8388607))
		(define m (list (bit:and fr 65535) (bit:shr fr 16)))
		(if (= be 255) (p:die "an infinite or NaN float in a constant") nil)
		(if (= be 0) (p:pow2rat s m -149 p:FLOAT)
		(p:pow2rat s (p:badd m (p:bpow2 23) 0 (list)) (- be 150) p:FLOAT)))))
(define p:w64rat (lambda (lo hi)
	(begin
		(define s (if (= 0 (bit:and hi p:SIGN32)) 0 1))
		(define be (bit:and (bit:shr hi 20) 2047))
		(define m (list (bit:and lo 65535) (bit:and (bit:shr lo 16) 65535)
			(bit:and hi 65535) (bit:and (bit:shr hi 16) 15)))
		(if (= be 2047) (p:die "an infinite or NaN double in a constant") nil)
		(if (= be 0) (p:pow2rat s m -1074 p:DOUBLE)
		(p:pow2rat s (p:badd m (p:bpow2 52) 0 (list)) (- be 1075) p:DOUBLE)))))
;; a float or double literal's value: (num BITS FLOAT), (num BITS DOUBLE)
;; on a 64-bit target, (llnum LO HI DOUBLE) on a 32-bit one
(define p:litrat (lambda (it)
	(if (= (head it) 'llnum) (p:w64rat (p:second it) (p:third it))
	(p:cfnum it))))
;; an initializer for an object of type t from a folded float (c4lc-gen's
;; g:fpinit): its bits, or the integer C's conversion makes of it
(define p:fvalinit (lambda (r t)
	(if (= t p:FLOAT) (list 'num (p:rat32 r) p:FLOAT)
	(if (= t p:DOUBLE)
		(begin
			(define d (p:rat64 r))
			(if (= p:WORD 8) (list 'num (bit:or (bit:shl (p:second d) 32) (bit:and (head d) p:M32)) p:DOUBLE)
			(list 'llnum (head d) (p:second d) p:DOUBLE)))
	(if (p:intty? t) (list 'num (p:convval (p:rat2int r) t))
	(p:die "a floating constant initializing a pointer or a struct"))))))

;; does the unit say float or double, or write a float literal? Then the
;; helpers its float arithmetic calls are spliced after it (p:fpsrc), as
;; the long long ones are -- and on a 32-bit target, since the helpers
;; say long long, those come too.
(define p:saysfp? (lambda (ts)
	(if (empty? ts) false
	(begin
		(define k (head (head ts)))
		(if (if (= k 'Float) true (if (= k 'Double) true (p:fptok? k))) true
		(next p:saysfp? (tail ts)))))))
(define p:withfp (lambda (tokens)
	(if (p:saysfp? tokens)
		(begin
			(define pp lex:pp)
			(set! lex:pp false)
			(define hs (lex:string p:fpsrc))
			(set! lex:pp pp)
			(+ (p:droplast tokens (list)) hs))
	tokens)))

;; ---- p:fpsrc: generated from src/c4fp/softfp.c by src/c4fp/mkfpsrc.py ----
;; What c4lc-gen calls for float and double (docs/c4-float.md): spliced
;; after a unit that says float or double, callers first, all static.
(define p:fpsrc "
static unsigned int __c4d_to_f(unsigned long long a) {
    if (__c4fp_hw()) return __c4_fpu(0, 0, &a, 26);
    return __c4d_to_f_s(a);
}
static unsigned int __c4d_to_f_s(unsigned long long a) {
    int e;
    unsigned int sign, f;
    unsigned long long sig;
    sign = (unsigned int)(a >> 63);
    e = (int)(a >> 52) & 0x7FF; sig = a & 0x000FFFFFFFFFFFFFull;
    if (e == 0x7FF) {
        if (sig) return __c4f_pack(sign, 0xFF, 0x00400000 | (unsigned int)(sig >> 29));
        return __c4f_pack(sign, 0xFF, 0);
    }
    f = (unsigned int)__c4fp_jam64(sig, 22);
    if (!(e | f)) return __c4f_pack(sign, 0, 0);
    return __c4f_round(sign, e - 0x381, f | 0x40000000);
}
static unsigned long long __c4f_to_d(unsigned int a) {
    unsigned long long r;
    if (__c4fp_hw()) { __c4_fpu(&r, 0, a, 25); return r; }
    return __c4f_to_d_s(a);
}
static unsigned long long __c4f_to_d_s(unsigned int a) {
    int e;
    unsigned int sign, sig;
    sign = a >> 31;
    e = (a >> 23) & 0xFF; sig = a & 0x007FFFFF;
    if (e == 0xFF) {
        if (sig) return __c4d_pack(sign, 0x7FF, ((unsigned long long)sig << 29) | 0x0008000000000000ull);
        return __c4d_pack(sign, 0x7FF, 0);
    }
    if (!e) {
        if (!sig) return __c4d_pack(sign, 0, 0);
        e = __c4f_normsub(&sig) - 1;
    }
    return __c4d_pack(sign, e + 0x380, (unsigned long long)sig << 29);
}
static unsigned int __c4d_to_u(unsigned long long a) {
    unsigned long long v;
    v = __c4d_to_ull(a);
    if (v > 0xFFFFFFFF) return 0xFFFFFFFF;
    return (unsigned int)v;
}
static int __c4d_to_i(unsigned long long a) {
    if (__c4fp_hw()) return __c4_fpu(0, 0, &a, 24);
    return __c4d_to_i_s(a);
}
static int __c4d_to_i_s(unsigned long long a) {
    long long v;
    v = __c4d_to_ll(a);
    if (sizeof(int) == 4 && (v < 0 - 2147483647 - 1 || v > 2147483647)) return 0 - 2147483647 - 1;
    return (int)v;
}
static unsigned long long __c4d_to_ull(unsigned long long a) {
    int e, sh;
    unsigned long long m;
    e = (int)(a >> 52) & 0x7FF;
    m = (a & 0x000FFFFFFFFFFFFFull) | 0x0010000000000000ull;
    sh = 0x433 - e;
    if (sh >= 53) return 0;
    if ((a >> 63) || sh < 0 - 11) return 0xFFFFFFFFFFFFFFFFull;
    if (sh <= 0) return m << (0 - sh);
    return m >> sh;
}
static long long __c4d_to_ll(unsigned long long a) {
    int e, sh;
    unsigned long long m;
    e = (int)(a >> 52) & 0x7FF;
    m = (a & 0x000FFFFFFFFFFFFFull) | 0x0010000000000000ull;
    sh = 0x433 - e;
    if (sh <= 0) {
        if (sh < 0 - 10) return (long long)0x8000000000000000ull;
        m = m << (0 - sh);
    } else {
        if (sh >= 53) return 0;
        m = m >> sh;
    }
    if (a >> 63) return 0 - (long long)m;
    return (long long)m;
}
static unsigned long long __c4d_from_u(unsigned int a) { return __c4d_from_ull_s((unsigned long long)a, 0); }
static unsigned long long __c4d_from_i(int a) {
    unsigned long long r;
    if (__c4fp_hw()) { __c4_fpu(&r, 0, a, 23); return r; }
    return __c4d_from_i_s(a);
}
static unsigned long long __c4d_from_i_s(int a) { return __c4d_from_ll((long long)a); }
static unsigned long long __c4d_from_ll(long long a) {
    if (a < 0) return __c4d_from_ull_s(0 - (unsigned long long)a, 1);
    return __c4d_from_ull_s((unsigned long long)a, 0);
}
static unsigned long long __c4d_from_ull(unsigned long long a) { return __c4d_from_ull_s(a, 0); }
static unsigned long long __c4d_from_ull_s(unsigned long long a, unsigned int sign) {
    if (!a) return __c4d_pack(sign, 0, 0);
    if (a >> 63) return __c4d_round(sign, 0x43D, __c4fp_jam64(a, 1));
    return __c4d_normround(sign, 0x43C, a);
}
static unsigned long long __c4d_abs(unsigned long long a) { return a & 0x7FFFFFFFFFFFFFFFull; }
static unsigned long long __c4d_neg(unsigned long long a) { return a ^ 0x8000000000000000ull; }
static int __c4d_cmp(unsigned long long a, unsigned long long b) {
    if (__c4fp_hw()) return __c4_fpu(0, &b, &a, 22);
    return __c4d_cmp_s(a, b);
}
static int __c4d_cmp_s(unsigned long long a, unsigned long long b) {
    if (__c4d_isnan(a) || __c4d_isnan(b)) return 2;
    if (!((a | b) & 0x7FFFFFFFFFFFFFFFull)) return 0;
    if (a == b) return 0;
    if ((a ^ b) >> 63) return (a >> 63) ? 0 - 1 : 1;
    if ((a < b) ^ (unsigned int)(a >> 63)) return 0 - 1;
    return 1;
}
static unsigned long long __c4d_sqrt(unsigned long long a) {
    unsigned long long r;
    if (__c4fp_hw()) { __c4_fpu(&r, 0, &a, 21); return r; }
    return __c4d_sqrt_s(a);
}
static unsigned long long __c4d_sqrt_s(unsigned long long a) {
    int ea, e, i, pos;
    unsigned long long sa, rhi, rlo, rem, root, trial, pair;
    ea = (int)(a >> 52) & 0x7FF; sa = a & 0x000FFFFFFFFFFFFFull;
    if (ea == 0x7FF) {
        if (sa) return a | 0x0008000000000000ull;
        if (!(a >> 63)) return a;
        return 0xFFF8000000000000ull;
    }
    if (a >> 63) {
        if (!((unsigned long long)ea | sa)) return a;
        return 0xFFF8000000000000ull;
    }
    if (!ea) { if (!sa) return a; ea = __c4d_normsub(&sa); }
    e = ea - 0x3FF;
    sa = sa | 0x0010000000000000ull;
    if (e & 1) { rhi = sa >> 7; rlo = sa << 57; e = e - 1; }
    else { rhi = sa >> 8; rlo = sa << 56; }
    rem = 0; root = 0;
    i = 54;
    while (i >= 0) {
        pos = 2 * i;
        if (pos >= 64) pair = (rhi >> (pos - 64)) & 3;
        else pair = (rlo >> pos) & 3;
        rem = (rem << 2) | pair;
        trial = (root << 2) | 1;
        root = root << 1;
        if (rem >= trial) { rem = rem - trial; root = root | 1; }
        --i;
    }
    return __c4d_round(0, e / 2 + 0x3FE, (root << 8) | (rem != 0));
}
static unsigned long long __c4d_div(unsigned long long a, unsigned long long b) {
    unsigned long long r;
    if (__c4fp_hw()) { __c4_fpu(&r, &b, &a, 20); return r; }
    return __c4d_div_s(a, b);
}
static unsigned long long __c4d_div_s(unsigned long long a, unsigned long long b) {
    int ea, eb, ez, i;
    unsigned int sign;
    unsigned long long sa, sb, q;
    sign = (unsigned int)((a ^ b) >> 63);
    ea = (int)(a >> 52) & 0x7FF; sa = a & 0x000FFFFFFFFFFFFFull;
    eb = (int)(b >> 52) & 0x7FF; sb = b & 0x000FFFFFFFFFFFFFull;
    if (ea == 0x7FF) {
        if (sa) return __c4d_nan2(a, b);
        if (eb == 0x7FF) { if (sb) return __c4d_nan2(a, b); return 0xFFF8000000000000ull; }
        return __c4d_pack(sign, 0x7FF, 0);
    }
    if (eb == 0x7FF) { if (sb) return __c4d_nan2(a, b); return __c4d_pack(sign, 0, 0); }
    if (!eb) {
        if (!sb) {
            if (!((unsigned long long)ea | sa)) return 0xFFF8000000000000ull;
            return __c4d_pack(sign, 0x7FF, 0);
        }
        eb = __c4d_normsub(&sb);
    }
    if (!ea) { if (!sa) return __c4d_pack(sign, 0, 0); ea = __c4d_normsub(&sa); }
    ez = ea - eb + 0x3FE;
    sa = sa | 0x0010000000000000ull;
    sb = sb | 0x0010000000000000ull;
    if (sa < sb) { ez = ez - 1; sa = sa << 1; }
    q = 0;
    i = 0;
    while (i < 63) {
        q = q << 1;
        if (sa >= sb) { sa = sa - sb; q = q | 1; }
        sa = sa << 1;
        ++i;
    }
    return __c4d_round(sign, ez, q | (sa != 0));
}
static unsigned long long __c4d_mul(unsigned long long a, unsigned long long b) {
    unsigned long long r;
    if (__c4fp_hw()) { __c4_fpu(&r, &b, &a, 19); return r; }
    return __c4d_mul_s(a, b);
}
static unsigned long long __c4d_mul_s(unsigned long long a, unsigned long long b) {
    int ea, eb, ez;
    unsigned int sign, a0, a1, b0, b1;
    unsigned long long sa, sb, sz, mag, p00, p01, p10, p11, mid, hi, lo;
    sign = (unsigned int)((a ^ b) >> 63);
    ea = (int)(a >> 52) & 0x7FF; sa = a & 0x000FFFFFFFFFFFFFull;
    eb = (int)(b >> 52) & 0x7FF; sb = b & 0x000FFFFFFFFFFFFFull;
    if (ea == 0x7FF) {
        if (sa || (eb == 0x7FF && sb)) return __c4d_nan2(a, b);
        mag = (unsigned long long)eb | sb;
        if (!mag) return 0xFFF8000000000000ull;
        return __c4d_pack(sign, 0x7FF, 0);
    }
    if (eb == 0x7FF) {
        if (sb) return __c4d_nan2(a, b);
        mag = (unsigned long long)ea | sa;
        if (!mag) return 0xFFF8000000000000ull;
        return __c4d_pack(sign, 0x7FF, 0);
    }
    if (!ea) { if (!sa) return __c4d_pack(sign, 0, 0); ea = __c4d_normsub(&sa); }
    if (!eb) { if (!sb) return __c4d_pack(sign, 0, 0); eb = __c4d_normsub(&sb); }
    ez = ea + eb - 0x3FF;
    sa = (sa | 0x0010000000000000ull) << 10;
    sb = (sb | 0x0010000000000000ull) << 11;
    a0 = (unsigned int)sa; a1 = (unsigned int)(sa >> 32);
    b0 = (unsigned int)sb; b1 = (unsigned int)(sb >> 32);
    p00 = (unsigned long long)a0 * b0;
    p01 = (unsigned long long)a0 * b1;
    p10 = (unsigned long long)a1 * b0;
    p11 = (unsigned long long)a1 * b1;
    mid = (p00 >> 32) + (unsigned int)p01 + (unsigned int)p10;
    lo = (mid << 32) | (unsigned int)p00;
    hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    sz = hi | (lo != 0);
    if (sz < 0x4000000000000000ull) { ez = ez - 1; sz = sz << 1; }
    return __c4d_round(sign, ez, sz);
}
static int __c4d_normsub(unsigned long long *sig) {
    int sh;
    sh = __c4fp_clz64(*sig) - 11;
    *sig = *sig << sh;
    return 1 - sh;
}
static unsigned long long __c4d_sub(unsigned long long a, unsigned long long b) {
    unsigned long long r;
    if (__c4fp_hw()) { __c4_fpu(&r, &b, &a, 18); return r; }
    return __c4d_sub_s(a, b);
}
static unsigned long long __c4d_sub_s(unsigned long long a, unsigned long long b) {
    unsigned int sa;
    sa = (unsigned int)(a >> 63);
    if (sa != (unsigned int)(b >> 63)) return __c4d_add_mags(a, b, sa);
    return __c4d_sub_mags(a, b, sa);
}
static unsigned long long __c4d_add(unsigned long long a, unsigned long long b) {
    unsigned long long r;
    if (__c4fp_hw()) { __c4_fpu(&r, &b, &a, 17); return r; }
    return __c4d_add_s(a, b);
}
static unsigned long long __c4d_add_s(unsigned long long a, unsigned long long b) {
    unsigned int sa;
    sa = (unsigned int)(a >> 63);
    if (sa != (unsigned int)(b >> 63)) return __c4d_sub_mags(a, b, sa);
    return __c4d_add_mags(a, b, sa);
}
static unsigned long long __c4d_sub_mags(unsigned long long a, unsigned long long b, unsigned int sign) {
    int ea, eb, ez, d, sh;
    unsigned long long sa, sb, sz, diff;
    ea = (int)(a >> 52) & 0x7FF; sa = a & 0x000FFFFFFFFFFFFFull;
    eb = (int)(b >> 52) & 0x7FF; sb = b & 0x000FFFFFFFFFFFFFull;
    d = ea - eb;
    if (!d) {
        if (ea == 0x7FF) { if (sa | sb) return __c4d_nan2(a, b); return 0xFFF8000000000000ull; }
        if (sa == sb) return 0;
        if (ea) ea = ea - 1;
        if (sa < sb) { sign = sign ^ 1; diff = sb - sa; } else diff = sa - sb;
        sh = __c4fp_clz64(diff) - 11;
        ez = ea - sh;
        if (ez < 0) { sh = ea; ez = 0; }
        return __c4d_pack(sign, ez, diff << sh);
    }
    sa = sa << 10; sb = sb << 10;
    if (d < 0) {
        sign = sign ^ 1;
        if (eb == 0x7FF) { if (sb) return __c4d_nan2(a, b); return __c4d_pack(sign, 0x7FF, 0); }
        sa = sa + (ea ? 0x4000000000000000ull : sa);
        sa = __c4fp_jam64(sa, 0 - d);
        sb = sb | 0x4000000000000000ull;
        ez = eb;
        sz = sb - sa;
    } else {
        if (ea == 0x7FF) { if (sa) return __c4d_nan2(a, b); return a; }
        sb = sb + (eb ? 0x4000000000000000ull : sb);
        sb = __c4fp_jam64(sb, d);
        sa = sa | 0x4000000000000000ull;
        ez = ea;
        sz = sa - sb;
    }
    return __c4d_normround(sign, ez - 1, sz);
}
static unsigned long long __c4d_add_mags(unsigned long long a, unsigned long long b, unsigned int sign) {
    int ea, eb, ez, d;
    unsigned long long sa, sb, sz;
    ea = (int)(a >> 52) & 0x7FF; sa = a & 0x000FFFFFFFFFFFFFull;
    eb = (int)(b >> 52) & 0x7FF; sb = b & 0x000FFFFFFFFFFFFFull;
    d = ea - eb;
    if (!d) {
        if (!ea) return a + sb;
        if (ea == 0x7FF) { if (sa | sb) return __c4d_nan2(a, b); return a; }
        ez = ea;
        sz = 0x0020000000000000ull + sa + sb;
        sz = sz << 9;
    } else {
        sa = sa << 9; sb = sb << 9;
        if (d < 0) {
            if (eb == 0x7FF) { if (sb) return __c4d_nan2(a, b); return __c4d_pack(sign, 0x7FF, 0); }
            ez = eb;
            if (ea) sa = sa + 0x2000000000000000ull; else sa = sa << 1;
            sa = __c4fp_jam64(sa, 0 - d);
        } else {
            if (ea == 0x7FF) { if (sa) return __c4d_nan2(a, b); return a; }
            ez = ea;
            if (eb) sb = sb + 0x2000000000000000ull; else sb = sb << 1;
            sb = __c4fp_jam64(sb, d);
        }
        sz = 0x2000000000000000ull + sa + sb;
        if (sz < 0x4000000000000000ull) { ez = ez - 1; sz = sz << 1; }
    }
    return __c4d_round(sign, ez, sz);
}
static unsigned long long __c4d_nan2(unsigned long long a, unsigned long long b) {
    if (__c4d_isnan(a)) return a | 0x0008000000000000ull;
    return b | 0x0008000000000000ull;
}
static int __c4d_isnan(unsigned long long a) { return (a & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull; }
static unsigned long long __c4d_normround(unsigned int sign, int exp, unsigned long long sig) {
    int sh;
    sh = __c4fp_clz64(sig) - 1;
    exp = exp - sh;
    if (sh >= 10 && (unsigned int)exp < 0x7FD)
        return __c4d_pack(sign, sig ? exp : 0, sig << (sh - 10));
    return __c4d_round(sign, exp, sig << sh);
}
static unsigned long long __c4d_round(unsigned int sign, int exp, unsigned long long sig) {
    unsigned int rbits;
    rbits = (unsigned int)sig & 0x3FF;
    if ((unsigned int)exp >= 0x7FD) {
        if (exp < 0) {
            sig = __c4fp_jam64(sig, 0 - exp);
            exp = 0;
            rbits = (unsigned int)sig & 0x3FF;
        } else if (exp > 0x7FD || sig + 0x200 >= 0x8000000000000000ull) {
            return __c4d_pack(sign, 0x7FF, 0);
        }
    }
    sig = (sig + 0x200) >> 10;
    if (rbits == 0x200) sig = sig & ~(unsigned long long)1;
    if (!sig) exp = 0;
    return __c4d_pack(sign, exp, sig);
}
static unsigned long long __c4d_pack(unsigned int sign, int exp, unsigned long long sig) {
    return ((unsigned long long)sign << 63) + ((unsigned long long)(unsigned int)exp << 52) + sig;
}
static unsigned int __c4f_to_u(unsigned int a) {
    unsigned long long v;
    v = __c4f_to_ull(a);
    if (v > 0xFFFFFFFF) return 0xFFFFFFFF;
    return (unsigned int)v;
}
static int __c4f_to_i(unsigned int a) {
    if (__c4fp_hw()) return __c4_fpu(0, 0, a, 8);
    return __c4f_to_i_s(a);
}
static int __c4f_to_i_s(unsigned int a) {
    long long v;
    v = __c4f_to_ll(a);
    if (sizeof(int) == 4 && (v < 0 - 2147483647 - 1 || v > 2147483647)) return 0 - 2147483647 - 1;
    return (int)v;
}
static unsigned long long __c4f_to_ull(unsigned int a) {
    int e, sh;
    unsigned int sig;
    unsigned long long m;
    e = (a >> 23) & 0xFF; sig = a & 0x007FFFFF;
    sh = 0xBE - e;
    if (sh >= 64 || !(e | sig)) return 0;
    if ((a >> 31) || sh < 0) return 0xFFFFFFFFFFFFFFFFull;
    m = (unsigned long long)(sig | 0x00800000) << 40;
    return m >> sh;
}
static long long __c4f_to_ll(unsigned int a) {
    int e, sh;
    unsigned int sig;
    unsigned long long m;
    e = (a >> 23) & 0xFF; sig = a & 0x007FFFFF;
    sh = 0xBE - e;
    if (sh <= 0) return (long long)0x8000000000000000ull;
    if (sh >= 64) return 0;
    m = (unsigned long long)(sig | 0x00800000) << 40;
    m = m >> sh;
    if (a >> 31) return 0 - (long long)m;
    return (long long)m;
}
static unsigned int __c4f_from_u(unsigned int a) { return __c4f_from_ull_s((unsigned long long)a, 0); }
static unsigned int __c4f_from_i(int a) {
    if (__c4fp_hw()) return __c4_fpu(0, 0, a, 7);
    return __c4f_from_i_s(a);
}
static unsigned int __c4f_from_i_s(int a) { return __c4f_from_ll((long long)a); }
static unsigned int __c4f_from_ll(long long a) {
    if (a < 0) return __c4f_from_ull_s(0 - (unsigned long long)a, 1);
    return __c4f_from_ull_s((unsigned long long)a, 0);
}
static unsigned int __c4f_from_ull(unsigned long long a) { return __c4f_from_ull_s(a, 0); }
static unsigned int __c4f_from_ull_s(unsigned long long a, unsigned int sign) {
    int sh;
    unsigned int sig;
    sh = __c4fp_clz64(a) - 40;
    if (sh >= 0) {
        if (!a) return __c4f_pack(sign, 0, 0);
        return __c4f_pack(sign, 0x95 - sh, (unsigned int)a << sh);
    }
    sh = sh + 7;
    if (sh < 0) sig = (unsigned int)__c4fp_jam64(a, 0 - sh);
    else sig = (unsigned int)a << sh;
    return __c4f_round(sign, 0x9C - sh, sig);
}
static unsigned int __c4f_abs(unsigned int a) { return a & 0x7FFFFFFF; }
static unsigned int __c4f_neg(unsigned int a) { return a ^ 0x80000000; }
static int __c4f_cmp(unsigned int a, unsigned int b) {
    if (__c4fp_hw()) return __c4_fpu(0, b, a, 6);
    return __c4f_cmp_s(a, b);
}
static int __c4f_cmp_s(unsigned int a, unsigned int b) {
    if (__c4f_isnan(a) || __c4f_isnan(b)) return 2;
    if (!((a | b) & 0x7FFFFFFF)) return 0;
    if (a == b) return 0;
    if ((a ^ b) >> 31) return (a >> 31) ? 0 - 1 : 1;
    if ((a < b) ^ (a >> 31)) return 0 - 1;
    return 1;
}
static unsigned int __c4f_sqrt(unsigned int a) {
    if (__c4fp_hw()) return __c4_fpu(0, 0, a, 5);
    return __c4f_sqrt_s(a);
}
static unsigned int __c4f_sqrt_s(unsigned int a) {
    int ea, e, i;
    unsigned int sa, rem, root, trial;
    unsigned long long r;
    ea = (a >> 23) & 0xFF; sa = a & 0x007FFFFF;
    if (ea == 0xFF) {
        if (sa) return a | 0x00400000;
        if (!(a >> 31)) return a;
        return 0xFFC00000;
    }
    if (a >> 31) {
        if (!(ea | sa)) return a;
        return 0xFFC00000;
    }
    if (!ea) { if (!sa) return a; ea = __c4f_normsub(&sa); }
    e = ea - 0x7F;
    sa = sa | 0x00800000;
    if (e & 1) { r = (unsigned long long)sa << 28; e = e - 1; }
    else r = (unsigned long long)sa << 27;
    rem = 0; root = 0;
    i = 25;
    while (i >= 0) {
        rem = (rem << 2) | ((unsigned int)(r >> (2 * i)) & 3);
        trial = (root << 2) | 1;
        root = root << 1;
        if (rem >= trial) { rem = rem - trial; root = root | 1; }
        --i;
    }
    return __c4f_round(0, e / 2 + 0x7E, (root << 5) | (rem != 0));
}
static unsigned int __c4f_div(unsigned int a, unsigned int b) {
    if (__c4fp_hw()) return __c4_fpu(0, b, a, 4);
    return __c4f_div_s(a, b);
}
static unsigned int __c4f_div_s(unsigned int a, unsigned int b) {
    int ea, eb, ez, i;
    unsigned int sa, sb, q, sign;
    sign = (a ^ b) >> 31;
    ea = (a >> 23) & 0xFF; sa = a & 0x007FFFFF;
    eb = (b >> 23) & 0xFF; sb = b & 0x007FFFFF;
    if (ea == 0xFF) {
        if (sa) return __c4f_nan2(a, b);
        if (eb == 0xFF) { if (sb) return __c4f_nan2(a, b); return 0xFFC00000; }
        return __c4f_pack(sign, 0xFF, 0);
    }
    if (eb == 0xFF) { if (sb) return __c4f_nan2(a, b); return __c4f_pack(sign, 0, 0); }
    if (!eb) {
        if (!sb) {
            if (!(ea | sa)) return 0xFFC00000;
            return __c4f_pack(sign, 0xFF, 0);
        }
        eb = __c4f_normsub(&sb);
    }
    if (!ea) { if (!sa) return __c4f_pack(sign, 0, 0); ea = __c4f_normsub(&sa); }
    ez = ea - eb + 0x7E;
    sa = sa | 0x00800000;
    sb = sb | 0x00800000;
    if (sa < sb) { ez = ez - 1; sa = sa << 1; }
    q = 0;
    i = 0;
    while (i < 31) {
        q = q << 1;
        if (sa >= sb) { sa = sa - sb; q = q | 1; }
        sa = sa << 1;
        ++i;
    }
    return __c4f_round(sign, ez, q | (sa != 0));
}
static unsigned int __c4f_mul(unsigned int a, unsigned int b) {
    if (__c4fp_hw()) return __c4_fpu(0, b, a, 3);
    return __c4f_mul_s(a, b);
}
static unsigned int __c4f_mul_s(unsigned int a, unsigned int b) {
    int ea, eb, ez;
    unsigned int sa, sb, sz, sign, mag;
    unsigned long long p;
    sign = (a ^ b) >> 31;
    ea = (a >> 23) & 0xFF; sa = a & 0x007FFFFF;
    eb = (b >> 23) & 0xFF; sb = b & 0x007FFFFF;
    if (ea == 0xFF) {
        if (sa || (eb == 0xFF && sb)) return __c4f_nan2(a, b);
        mag = eb | sb;
        if (!mag) return 0xFFC00000;
        return __c4f_pack(sign, 0xFF, 0);
    }
    if (eb == 0xFF) {
        if (sb) return __c4f_nan2(a, b);
        mag = ea | sa;
        if (!mag) return 0xFFC00000;
        return __c4f_pack(sign, 0xFF, 0);
    }
    if (!ea) { if (!sa) return __c4f_pack(sign, 0, 0); ea = __c4f_normsub(&sa); }
    if (!eb) { if (!sb) return __c4f_pack(sign, 0, 0); eb = __c4f_normsub(&sb); }
    ez = ea + eb - 0x7F;
    sa = (sa | 0x00800000) << 7;
    sb = (sb | 0x00800000) << 8;
    p = (unsigned long long)sa * sb;
    sz = (unsigned int)(p >> 32) | ((unsigned int)p != 0);
    if (sz < 0x40000000) { ez = ez - 1; sz = sz << 1; }
    return __c4f_round(sign, ez, sz);
}
static int __c4f_normsub(unsigned int *sig) {
    int sh;
    sh = __c4fp_clz32(*sig) - 8;
    *sig = *sig << sh;
    return 1 - sh;
}
static unsigned int __c4f_sub(unsigned int a, unsigned int b) {
    if (__c4fp_hw()) return __c4_fpu(0, b, a, 2);
    return __c4f_sub_s(a, b);
}
static unsigned int __c4f_sub_s(unsigned int a, unsigned int b) {
    if ((a ^ b) >> 31) return __c4f_add_mags(a, b, a >> 31);
    return __c4f_sub_mags(a, b, a >> 31);
}
static unsigned int __c4f_add(unsigned int a, unsigned int b) {
    if (__c4fp_hw()) return __c4_fpu(0, b, a, 1);
    return __c4f_add_s(a, b);
}
static unsigned int __c4f_add_s(unsigned int a, unsigned int b) {
    if ((a ^ b) >> 31) return __c4f_sub_mags(a, b, a >> 31);
    return __c4f_add_mags(a, b, a >> 31);
}
static unsigned int __c4f_sub_mags(unsigned int a, unsigned int b, unsigned int sign) {
    int ea, eb, ez, d, sh;
    unsigned int sa, sb, sx, sy;
    int diff;
    ea = (a >> 23) & 0xFF; sa = a & 0x007FFFFF;
    eb = (b >> 23) & 0xFF; sb = b & 0x007FFFFF;
    d = ea - eb;
    if (!d) {
        if (ea == 0xFF) { if (sa | sb) return __c4f_nan2(a, b); return 0xFFC00000; }
        diff = (int)sa - (int)sb;
        if (!diff) return 0;
        if (ea) ea = ea - 1;
        if (diff < 0) { sign = sign ^ 1; diff = 0 - diff; }
        sh = __c4fp_clz32((unsigned int)diff) - 8;
        ez = ea - sh;
        if (ez < 0) { sh = ea; ez = 0; }
        return __c4f_pack(sign, ez, (unsigned int)diff << sh);
    }
    sa = sa << 7; sb = sb << 7;
    if (d < 0) {
        sign = sign ^ 1;
        if (eb == 0xFF) { if (sb) return __c4f_nan2(a, b); return __c4f_pack(sign, 0xFF, 0); }
        ez = eb - 1;
        sx = sb | 0x40000000;
        sy = sa + (ea ? 0x40000000 : sa);
        d = 0 - d;
    } else {
        if (ea == 0xFF) { if (sa) return __c4f_nan2(a, b); return a; }
        ez = ea - 1;
        sx = sa | 0x40000000;
        sy = sb + (eb ? 0x40000000 : sb);
    }
    return __c4f_normround(sign, ez, sx - __c4fp_jam32(sy, d));
}
static unsigned int __c4f_add_mags(unsigned int a, unsigned int b, unsigned int sign) {
    int ea, eb, ez, d;
    unsigned int sa, sb, sz;
    ea = (a >> 23) & 0xFF; sa = a & 0x007FFFFF;
    eb = (b >> 23) & 0xFF; sb = b & 0x007FFFFF;
    d = ea - eb;
    if (!d) {
        if (!ea) return a + sb;
        if (ea == 0xFF) { if (sa | sb) return __c4f_nan2(a, b); return a; }
        ez = ea;
        sz = 0x01000000 + sa + sb;
        if (!(sz & 1) && ez < 0xFE) return __c4f_pack(sign, ez, sz >> 1);
        sz = sz << 6;
    } else {
        sa = sa << 6; sb = sb << 6;
        if (d < 0) {
            if (eb == 0xFF) { if (sb) return __c4f_nan2(a, b); return __c4f_pack(sign, 0xFF, 0); }
            ez = eb;
            sa = sa + (ea ? 0x20000000 : sa);
            sa = __c4fp_jam32(sa, 0 - d);
        } else {
            if (ea == 0xFF) { if (sa) return __c4f_nan2(a, b); return a; }
            ez = ea;
            sb = sb + (eb ? 0x20000000 : sb);
            sb = __c4fp_jam32(sb, d);
        }
        sz = 0x20000000 + sa + sb;
        if (sz < 0x40000000) { ez = ez - 1; sz = sz << 1; }
    }
    return __c4f_round(sign, ez, sz);
}
static unsigned int __c4f_nan2(unsigned int a, unsigned int b) {
    if (__c4f_isnan(a)) return a | 0x00400000;
    return b | 0x00400000;
}
static int __c4f_isnan(unsigned int a) { return (a & 0x7FFFFFFF) > 0x7F800000; }
static unsigned int __c4f_normround(unsigned int sign, int exp, unsigned int sig) {
    int sh;
    sh = __c4fp_clz32(sig) - 1;
    exp = exp - sh;
    if (sh >= 7 && (unsigned int)exp < 0xFD)
        return __c4f_pack(sign, sig ? exp : 0, sig << (sh - 7));
    return __c4f_round(sign, exp, sig << sh);
}
static unsigned int __c4f_round(unsigned int sign, int exp, unsigned int sig) {
    unsigned int rbits;
    rbits = sig & 0x7F;
    if ((unsigned int)exp >= 0xFD) {
        if (exp < 0) {
            sig = __c4fp_jam32(sig, 0 - exp);
            exp = 0;
            rbits = sig & 0x7F;
        } else if (exp > 0xFD || sig + 0x40 >= 0x80000000) {
            return __c4f_pack(sign, 0xFF, 0);
        }
    }
    sig = (sig + 0x40) >> 7;
    if (rbits == 0x40) sig = sig & ~1;
    if (!sig) exp = 0;
    return __c4f_pack(sign, exp, sig);
}
static unsigned int __c4f_pack(unsigned int sign, int exp, unsigned int sig) {
    return (sign << 31) + ((unsigned int)exp << 23) + sig;
}
static unsigned long long __c4fp_jam64(unsigned long long a, int dist) {
    if (dist <= 0) return a;
    if (dist < 63) return (a >> dist) | ((a << (64 - dist)) != 0);
    return a != 0;
}
static unsigned int __c4fp_jam32(unsigned int a, int dist) {
    if (dist <= 0) return a;
    if (dist < 31) return (a >> dist) | ((a << (32 - dist)) != 0);
    return a != 0;
}
static int __c4fp_clz64(unsigned long long a) {
    unsigned int hi;
    hi = (unsigned int)(a >> 32);
    if (hi) return __c4fp_clz32(hi);
    return 32 + __c4fp_clz32((unsigned int)a);
}
static int __c4fp_clz32(unsigned int a) {
    int n;
    if (!a) return 32;
    n = 0;
    if (!(a & 0xFFFF0000)) { n = n + 16; a = a << 16; }
    if (!(a & 0xFF000000)) { n = n + 8; a = a << 8; }
    if (!(a & 0xF0000000)) { n = n + 4; a = a << 4; }
    if (!(a & 0xC0000000)) { n = n + 2; a = a << 2; }
    if (!(a & 0x80000000)) n = n + 1;
    return n;
}
static int __c4fp_hw() {
    static int asked, has;
    if (!asked) { has = (__c4_info() & 0x10000) != 0; asked = 1; }
    return has;
}
")
;; ---- end of p:fpsrc ----

;; does the unit say `long long`, or write an LL literal?
(define p:saysll? (lambda (ts)
	(if (empty? ts) false
	(if (p:lltok? (head (head ts))) true
	(if (empty? (tail ts)) false
	(if (if (= (head (head ts)) 'Long) (= (head (head (tail ts))) 'Long) false) true
	(next p:saysll? (tail ts))))))))
(define p:withll (lambda (tokens)
	(if (if (= p:WORD 4) (p:saysll? tokens) false)
		(begin
			(define pp lex:pp)
			(set! lex:pp false)
			(define hs (lex:string p:llsrc))
			(set! lex:pp pp)
			(+ (p:droplast tokens (list)) hs))
	tokens)))
;; the tokens without their Eof
(define p:droplast (lambda (ts acc)
	(if (empty? (tail ts)) (p:reverse acc)
	(next p:droplast (tail ts) (p:cons (head ts) acc)))))

(define parse:program (lambda (tokens)
	(begin
		(set! p:toks (p:untag (p:withll (p:withfp tokens)) (list)))
		(set! p:enums (list))
		(set! p:sconsts (list))
		(set! p:locals (list))
		(set! p:structs (list))
		(set! p:slayouts (list))
		(set! p:gvars (list))
		(set! p:rows (list))
		(set! p:inner nil)
		(set! p:dyninits (list))
		(set! p:pack 0)
		(set! p:packs (list))
		(set! p:nstructs 0)
		(set! p:typedefs (list))
		(set! p:pending (list))
		(p:cons 'program (p:top (list))))))

)
