;; c4sc.lisp -- the c4sp Lisp, compiled to C.
;;
;;   c4sp -R src/c4sc/c4sc.lisp in.lisp out.c
;;
;; Written in the subset it compiles, so it can eventually compile
;; itself (M7). docs/c4sc-design.md has the survey that decides the
;; shape: across all 5,016 lines of c4lc there are no closures, no
;; higher-order calls, no variadic lambdas, no macros and no eval, so
;; every function is a C function and every local is a C local. This is
;; a transliterator, not a Scheme compiler.
;;
;; Output shape:
;;   globals            one per top-level (define x <not-a-lambda>)
;;   literal table      strings and quoted data, built once in sc_init
;;   prototypes         every function, so definition order stops mattering
;;   functions          one per top-level (define f (lambda ...))
;;   sc_init_<unit>()   the top-level defines, in source order
(begin

;; ---- text ------------------------------------------------------------
;; string:join renders ANY cell through cell_write, which is how an atom
;; becomes its name without a new builtin.
(define second (lambda (L) (index L 1)))
(define third (lambda (L) (index L 2)))

(define sc:text (lambda (X) (string:join "" (list X))))

;; c4sp's reader translates NO escapes: a "\n" in source is a backslash
;; and an n (read.h:6, "String contents are kept raw"). So every
;; character this compiler emits is built from its byte value, once.
(define sc:chr (lambda (C)
	(begin (define S (string:alloc 1)) (string:byte! S 0 C) S)))
(define sc:chr2 (lambda (A B)
	(begin (define S (string:alloc 2)) (string:byte! S 0 A) (string:byte! S 1 B) S)))
(define NL (sc:chr 10))
(define TAB (sc:chr 9))
(define E_dq (sc:chr2 92 34))
(define E_bs (sc:chr2 92 92))
(define E_nl (sc:chr2 92 110))
(define DQ (sc:chr 34))

;; ---- output ----------------------------------------------------------
(define sc:decls "")      ;; globals + prototypes
(define sc:funs "")       ;; function bodies
(define sc:inits "")      ;; sc_init statements
(define sc:body "")       ;; the function being emitted
(define sc:b (lambda (S) (set! sc:body (+ sc:body S))))

;; ---- name mangling ---------------------------------------------------
;; [A-Za-z0-9_] survives; anything else becomes _NN with the byte's
;; decimal code, which cannot collide and can be read back by eye.
(define sc:alnum? (lambda (C)
	(if (if (>= C 97) (<= C 122) false) true
	(if (if (>= C 65) (<= C 90) false) true
	(if (if (>= C 48) (<= C 57) false) true
	(= C 95))))))

(define sc:mang/2 (lambda (S I N Acc)
	(if (>= I N) Acc
		(begin
			(define C (string:byte S I))
			(next sc:mang/2 S (+ I 1) N
				(+ Acc (if (sc:alnum? C)
					(string:substr S I 1)
					(+ "_" (sc:text C)))))))))

(define sc:mang (lambda (Nm)
	(begin
		(define S (sc:text Nm))
		(+ "L_" (sc:mang/2 S 0 (length S) "")))))

;; ---- literals --------------------------------------------------------
;; Strings and quoted data are built ONCE, in sc_init, so a hot loop is
;; not re-reading its own constants. The table is a list of C statements
;; and the name is its index.
(define sc:qpfx "Q")
(define sc:defined nil)
(define sc:extvars nil)
(define sc:extdecls "")
(define sc:curlocals nil)
(define sc:called nil)
(define sc:nlit 0)
(define sc:litname (lambda () (+ sc:qpfx (sc:text sc:nlit))))

;; C string escaping: c4lc's lexer takes \" \\ and \n, which is all a
;; Lisp datum printed back can contain.
(define sc:esc/2 (lambda (S I N Acc)
	(if (>= I N) Acc
		(begin
			(define C (string:byte S I))
			(next sc:esc/2 S (+ I 1) N
				(+ Acc (if (= C 34) E_dq
					(if (= C 92) E_bs
					(if (= C 10) E_nl
						(string:substr S I 1))))))))))
(define sc:esc (lambda (S) (sc:esc/2 S 0 (length S) "")))

;; A literal cell -> the name of the global holding it.
;;
;; Quoted data is built STRUCTURALLY -- cons by cons, atom by atom --
;; rather than printed back and re-read. Printing loses the difference
;; between a string and an atom: c4lc-lex.lisp's keyword table is
;;     '(("char" Char) ("else" Else) ...)
;; and the round trip turned every "char" into the atom char, so every
;; keyword lexed as an identifier. The count was still right, which is
;; why the token DUMP is the bar and not the count.
(define sc:qlit (lambda (X)
	(begin
		(define T (typeof X))
		;; The end of a list is the NULL cell, and typeof calls it an
		;; atom -- the same answer it gives the atom nil -- so the check
		;; has to be empty? as well. Emitting sc_atom("nil") instead of 0
		;; built '(continue) as the improper list (continue . nil), which
		;; only showed up in the AST dump, in one place, as a printed dot.
		(if (if (= T 'atom) (empty? X) false) "0"
		(if (= T 'number) (+ "mk_int(" (+ (sc:text X) ")"))
		(if (= T 'string) (sc:strlit "sc_str" X)
		(if (= T 'list)
			(if (empty? X) "0"
				(+ "sc_cons(" (+ (sc:qlit (head X)) (+ ", " (+ (sc:qlit (tail X)) ")")))))
			(sc:strlit "sc_atom" X))))))))

(define sc:strlit (lambda (Fn X)
	(begin
		(define S (sc:text X))
		(+ Fn (+ "(" (+ DQ (+ (sc:esc S) (+ DQ (+ ", " (+ (sc:text (length S)) ")"))))))))))

(define sc:lit (lambda (X Str)
	(begin
		(define Nm (sc:litname))
		(set! sc:nlit (+ sc:nlit 1))
		(set! sc:decls (+ sc:decls (+ "int *" (+ Nm (+ ";" NL)))))
		(set! sc:inits (+ sc:inits
			(+ TAB (+ "gc_add_root((int *)&" (+ Nm (+ ");" NL))))))
		(set! sc:inits (+ sc:inits
			(+ TAB (+ Nm (+ " = "
				(+ (if Str (sc:strlit "sc_str" X) (sc:qlit X)) (+ ";" NL)))))))
		Nm)))

;; ---- temporaries -----------------------------------------------------
;; A temp is live only while the call that needs it is being built, so
;; the counter is marked and restored around every argument list. The
;; high-water mark becomes the function's declarations.
(define sc:temp 0)
(define sc:maxtemp 0)
(define sc:initmax 0)
(define sc:newtemp (lambda ()
	(begin
		(define T (+ "t" (sc:text sc:temp)))
		(set! sc:temp (+ sc:temp 1))
		(if (> sc:temp sc:maxtemp) (set! sc:maxtemp sc:temp) false)
		T)))

;; ---- the builtin table -----------------------------------------------
;; Name -> the scrt.h function that is that builtin without the call
;; protocol. false means "an ordinary user function".
(define sc:builtin (lambda (F)
	(if (= F '=)            "sc_eq"
	(if (= F '!=)           "sc_ne"
	(if (= F '<)            "sc_lt"
	(if (= F '<=)           "sc_le"
	(if (= F '>)            "sc_gt"
	(if (= F '>=)           "sc_ge"
	(if (= F 'not)          "sc_not"
	(if (= F 'head)         "sc_head"
	(if (= F 'tail)         "sc_tail"
	(if (= F 'empty?)       "sc_empty"
	(if (= F 'index)        "sc_index"
	(if (= F 'length)       "sc_length"
	(if (= F 'typeof)       "sc_typeof"
	(if (= F 'bit:and)      "sc_bitand"
	(if (= F 'bit:or)       "sc_bitor"
	(if (= F 'bit:xor)      "sc_bitxor"
	(if (= F 'bit:shl)      "sc_bitshl"
	(if (= F 'bit:shr)      "sc_bitshr"
	(if (= F 'string:alloc) "sc_str_alloc"
	(if (= F 'string:byte)  "sc_str_byte"
	(if (= F 'string:byte!) "sc_str_setbyte"
	(if (= F 'string:word)  "sc_str_word"
	(if (= F 'string:word!) "sc_str_setword"
	(if (= F 'string:substr) "sc_str_substr"
	(if (= F 'file:read)    "sc_file_read"
	(if (= F 'file:path)    "sc_file_path"
	(if (= F 'file:exists)  "sc_file_exists"
		false)))))))))))))))))))))))))))))

;; The arithmetic ones nest: (+ a b c) is sc_add(sc_add(a,b),c).
(define sc:arith (lambda (F)
	(if (= F '+) "sc_add"
	(if (= F '-) "sc_sub"
	(if (= F '*) "sc_mul"
	(if (= F '/) "sc_div"
		false))))))

;; ---- collecting a function's locals ----------------------------------
;; Every internal (define x ...) is a C local, and C4 has no mid-block
;; declarations, so they are gathered before the body is emitted.
(define sc:member? (lambda (X L)
	(if (empty? L) false
		(if (= (head L) X) true (next sc:member? X (tail L))))))

(define sc:locals/2 (lambda (X Acc)
	(if (= (typeof X) 'list)
		(if (empty? X) Acc
			(begin
				(define H (head X))
				(if (= H 'quote) Acc
					(begin
						(if (= H 'define)
							(if (sc:member? (second X) Acc) false
								(set! Acc (+ Acc (list (second X)))))
							false)
						(next sc:locals/2* (tail X) Acc)))))
		Acc)))
(define sc:locals/2* (lambda (L Acc)
	(if (empty? L) Acc
		(next sc:locals/2* (tail L) (sc:locals/2 (head L) Acc)))))
(define sc:locals (lambda (X) (sc:locals/2 X (list))))

;; Does this function tail-call itself? Only then does it need a loop.
(define sc:selfnext? (lambda (X Nm)
	(if (= (typeof X) 'list)
		(if (empty? X) false
			(begin
				(define H (head X))
				(if (= H 'quote) false
					(if (if (= H 'next) (= (second X) Nm) false) true
						(next sc:selfnext?* (tail X) Nm)))))
		false)))
(define sc:selfnext?* (lambda (L Nm)
	(if (empty? L) false
		(if (sc:selfnext? (head L) Nm) true (next sc:selfnext?* (tail L) Nm)))))

;; ---- external calls --------------------------------------------------
;; A name called here but defined in a sibling unit (c4opt calls c4r's
;; cons, second, reverse...) has to be DECLARED, because that is how
;; c4lc -c knows to make it an extern symbol for c4rlink to resolve.
;; The arity comes from the call site, which is the only place it is
;; written down.
(define sc:seen? (lambda (Nm L)
	(if (empty? L) false
		(if (= (head (head L)) Nm) true (next sc:seen? Nm (tail L))))))

(define sc:note-call (lambda (Nm N)
	(if (sc:seen? Nm sc:called) false
		(set! sc:called (+ sc:called (list (list Nm N)))))))

(define sc:special? (lambda (H)
	(if (= H 'if) true (if (= H 'begin) true (if (= H 'define) true
	(if (= H 'set!) true (if (= H 'lambda) true (if (= H 'quote) true
	(= H 'next)))))))))

(define sc:scan (lambda (X)
	(if (= (typeof X) 'list)
		(if (empty? X) false
			(begin
				(define H (head X))
				(if (= H 'quote) false
				;; a lambda's PARAMETER LIST is a list whose head is an
				;; atom and is not a call -- scanning it is how Op, Scan
				;; and Syms turned into imaginary external functions.
				(if (= H 'lambda) (sc:scan (third X))
					(begin
						(if (= (typeof H) 'atom)
							(if (sc:special? H) false
								(if (= (sc:builtin H) false)
									(if (= (sc:arith H) false)
										(if (= H 'list) false
										(if (= H 'print) false
										(if (= H 'error) false
											(sc:note-call H (length (tail X))))))
										false)
									false))
							false)
						(if (= H 'next)
							(sc:note-call (second X) (length (tail (tail X))))
							false)
						(sc:scan* (tail X)))))))
		false)))
(define sc:scan* (lambda (L)
	(if (empty? L) false
		(begin (sc:scan (head L)) (next sc:scan* (tail L))))))

(define sc:protoargs (lambda (I N Acc First)
	(if (>= I N) Acc
		(next sc:protoargs (+ I 1) N
			(+ Acc (+ (if First "" ", ") (+ "int *a" (sc:text I)))) false))))

(define sc:externs (lambda (L)
	(if (empty? L) false
		(begin
			(define E (head L))
			(if (sc:member? (head E) sc:defined) false
				(set! sc:decls (+ sc:decls
					(+ "int *" (+ (sc:mang (head E))
						(+ " (" (+ (sc:protoargs 0 (second E) "" true) (+ ");" NL))))))))
			(next sc:externs (tail L))))))

;; ---- expressions -----------------------------------------------------
;; Every expression compiles to statements that assign to Dst. That is
;; what makes `if` and `begin` -- which are expressions in Lisp and
;; statements in C -- fall out with no special casing.
;; A name that is neither a parameter, a local, nor defined in this unit
;; belongs to a sibling unit -- c4opt reads c4r.lisp's W. It has to be
;; DECLARED extern, or c4lc has nothing to hang a symbol patch on.
(define sc:noteref (lambda (X)
	(if (= X 'nil) false (if (= X 'true) false (if (= X 'false) false
	(if (sc:member? X sc:curparams) false
	(if (sc:member? X sc:curlocals) false
	(if (sc:member? X sc:defined) false
	(if (sc:member? X sc:extvars) false
		(begin
			(set! sc:extvars (+ sc:extvars (list X)))
			(set! sc:extdecls (+ sc:extdecls
				(+ "extern int *" (+ (sc:mang X) (+ ";" NL)))))))))))))))

(define sc:atomref (lambda (X Dst)
	(begin
	(sc:noteref X)
	(sc:b (+ TAB (+ Dst (+ " = "
		(+ (if (= X 'nil) "0"
			(if (= X 'true) "cell_true"
			(if (= X 'false) "cell_false"
				(sc:mang X))))
			(+ ";" NL)))))))))

(define sc:expr (lambda (X Dst)
	(begin
		(define T (typeof X))
		(if (= T 'number)
			(sc:b (+ TAB (+ Dst (+ " = mk_int(" (+ (sc:text X) (+ ");" NL))))))
		(if (= T 'string)
			(sc:b (+ TAB (+ Dst (+ " = " (+ (sc:lit X true) (+ ";" NL))))))
		(if (= T 'list)
			(if (empty? X) (sc:b (+ TAB (+ Dst (+ " = 0;" NL)))) (sc:form X Dst))
		(if (= T 'atom) (sc:atomref X Dst)
			(sc:b (+ TAB (+ Dst (+ " = " (+ (sc:lit X false) (+ ";" NL)))))))))))))

(define sc:seq (lambda (L Dst)
	(if (empty? L) (sc:b (+ TAB (+ Dst (+ " = 0;" NL))))
		(if (empty? (tail L)) (sc:expr (head L) Dst)
			(begin
				(sc:expr (head L) Dst)
				(next sc:seq (tail L) Dst))))))

;; An argument that is just a variable or a literal needs no temporary
;; and no statement of its own -- it can go straight into the call. That
;; is most of them, and without it every call site is three lines where
;; it should be one.
(define sc:simple (lambda (X)
	(begin
		(define T (typeof X))
		(if (= T 'number) (+ "mk_int(" (+ (sc:text X) ")"))
		(if (= T 'atom)
			(if (= X 'nil) "0"
			(if (= X 'true) "cell_true"
			(if (= X 'false) "cell_false"
				(begin (sc:noteref X) (sc:mang X)))))
			false)))))

(define sc:args (lambda (L Acc)
	(if (empty? L) Acc
		(begin
			(define S (sc:simple (head L)))
			(if (= S false)
				(begin
					(define T (sc:newtemp))
					(sc:expr (head L) T)
					(set! S T))
				false)
			(next sc:args (tail L) (+ Acc (list S)))))))

;; A SELF tail call may not inline its arguments, because assigning the
;; parameters happens in order and an argument may name a parameter that
;; an earlier assignment has already overwritten. c4opt:optloop is
;; exactly that shape -- (next c4opt:optloop ... (count Code) N ...)
;; passes the OLD N as Prev, and inlining it read the new one, so the
;; fixpoint loop ran one round and stopped. Every argument goes to a
;; temporary first; the assignments then cannot see each other.
(define sc:argsT (lambda (L Acc)
	(if (empty? L) Acc
		(begin
			(define T (sc:newtemp))
			(sc:expr (head L) T)
			(next sc:argsT (tail L) (+ Acc (list T)))))))

(define sc:commas (lambda (L Acc First)
	(if (empty? L) Acc
		(next sc:commas (tail L)
			(+ Acc (+ (if First "" ", ") (head L))) false))))

;; (+ a b c) nests left, which is what the interpreter's fold does.
(define sc:nest (lambda (Fn L Acc)
	(if (empty? L) Acc
		(next sc:nest Fn (tail L)
			(+ Fn (+ "(" (+ Acc (+ ", " (+ (head L) ")")))))))))

(define sc:call (lambda (X Dst)
	(begin
		(define F (head X))
		(define Mark sc:temp)
		(define As (sc:args (tail X) (list)))
		(define Ar (sc:arith F))
		(define B (sc:builtin F))
		(if (= F 'list)
			(sc:b (+ TAB (+ Dst (+ " = " (+ (sc:conses As) (+ ";" NL))))))
		(if (= F 'print)
			(sc:b (+ TAB (+ Dst (+ " = sc_print(" (+ (sc:conses As) (+ ");" NL))))))
		(if (= F 'error)
			(sc:b (+ TAB (+ Dst (+ " = sc_error(" (+ (sc:conses As) (+ ");" NL))))))
		(if (= Ar false)
			(sc:b (+ TAB (+ Dst (+ " = "
				(+ (if (= B false) (sc:mang F) B)
					(+ "(" (+ (sc:commas As "" true) (+ ");" NL))))))))
			(sc:b (+ TAB (+ Dst (+ " = "
				(+ (sc:nest Ar (tail As) (head As)) (+ ";" NL))))))))))
		(set! sc:temp Mark))))

(define sc:conses (lambda (L)
	(if (empty? L) "0"
		(+ "sc_cons(" (+ (head L) (+ ", " (+ (sc:conses (tail L)) ")")))))))

(define sc:if (lambda (X Dst)
	(begin
		(define Mark sc:temp)
		(define C (sc:simple (second X)))
		(if (= C false)
			(begin (set! C (sc:newtemp)) (sc:expr (second X) C))
			false)
		(set! sc:temp Mark)
		(sc:b (+ (+ TAB "if (sc_true(") (+ C (+ ")) {" NL))))
		(sc:expr (third X) Dst)
		(sc:b (+ TAB (+ "} else {" NL)))
		(if (empty? (tail (tail (tail X))))
			(sc:b (+ TAB (+ Dst (+ " = 0;" NL))))
			(sc:expr (index X 3) Dst))
		(sc:b (+ TAB (+ "}" NL))))))

(define sc:setform (lambda (X Dst)
	(begin
		;; the TARGET needs declaring too: c4lc-pp.lisp does
		;; (set! lex:pp true), and a store to a sibling unit's global is
		;; as much an extern reference as a load from one.
		(sc:noteref (second X))
		(sc:expr (third X) (sc:mang (second X)))
		(sc:b (+ TAB (+ Dst (+ " = " (+ (sc:mang (second X)) (+ ";" NL)))))))))

;; The current function, for self-tail-call detection.
(define sc:curname nil)
(define sc:curparams nil)

(define sc:assignparams (lambda (Ps Ts)
	(if (empty? Ps) false
		(begin
			(sc:b (+ TAB (+ (sc:mang (head Ps)) (+ " = " (+ (head Ts) (+ ";" NL))))))
			(next sc:assignparams (tail Ps) (tail Ts))))))

(define sc:next (lambda (X Dst)
	(begin
		(define F (second X))
		(if (= F sc:curname)
			(begin
				(define Mark sc:temp)
				(define As (sc:argsT (tail (tail X)) (list)))
				(sc:assignparams sc:curparams As)
				(set! sc:temp Mark)
				(sc:b (+ TAB (+ "continue;" NL))))
			(sc:call (tail X) Dst)))))

(define sc:form (lambda (X Dst)
	(begin
		(define H (head X))
		(if (= H 'if)     (sc:if X Dst)
		(if (= H 'begin)  (sc:seq (tail X) Dst)
		(if (= H 'define) (sc:setform (list 'set! (second X) (third X)) Dst)
		(if (= H 'set!)   (sc:setform X Dst)
		(if (= H 'next)   (sc:next X Dst)
		(if (= H 'quote)  (sc:b (+ TAB (+ Dst (+ " = " (+ (sc:lit (second X) false) (+ ";" NL))))))
			(sc:call X Dst))))))))))

;; ---- functions -------------------------------------------------------
(define sc:params (lambda (Ps Acc First)
	(if (empty? Ps) Acc
		(next sc:params (tail Ps)
			(+ Acc (+ (if First "" ", ") (+ "int *" (sc:mang (head Ps))))) false))))

(define sc:declist (lambda (L Acc)
	(if (empty? L) Acc
		(next sc:declist (tail L)
			(+ Acc (+ (+ TAB "int *") (+ (head L) (+ ";" NL))))))))

(define sc:tempdecls (lambda (I N Acc)
	(if (>= I N) Acc
		(next sc:tempdecls (+ I 1) N
			(+ Acc (+ (+ TAB "int *t") (+ (sc:text I) (+ ";" NL))))))))

(define sc:fn (lambda (Nm Lam)
	(begin
		(define Ps (second Lam))
		(define Body (third Lam))
		(define Sig "")
		(define Loop false)
		(define Locals nil)
		(define Out "")
		(set! Sig (+ "int *" (+ (sc:mang Nm) (+ " (" (+ (sc:params Ps "" true) ")")))))
		(set! Loop (sc:selfnext? Body Nm))
		(set! Locals (sc:manglist (sc:locals Body) (list)))
		(set! sc:curname Nm)
		(set! sc:curparams Ps)
		(set! sc:curlocals (sc:locals Body))
		(set! sc:temp 0)
		(set! sc:maxtemp 1)
		(set! sc:body "")
		(sc:expr Body "r")
		(set! sc:decls (+ sc:decls (+ Sig (+ ";" NL))))
		(set! Out (+ Sig NL))
		(set! Out (+ Out (+ "{" NL)))
		(set! Out (+ Out (+ TAB (+ "int *r;" NL))))
		(set! Out (+ Out (sc:declist Locals "")))
		(set! Out (+ Out (sc:tempdecls 0 sc:maxtemp "")))
		(if Loop (set! Out (+ Out (+ TAB (+ "while (1) {" NL)))) false)
		(set! Out (+ Out sc:body))
		(set! Out (+ Out (+ TAB (+ "return r;" NL))))
		(if Loop (set! Out (+ Out (+ TAB (+ "}" NL)))) false)
		(set! Out (+ Out (+ "}" (+ NL NL))))
		(set! sc:funs (+ sc:funs Out)))))

(define sc:manglist (lambda (L Acc)
	(if (empty? L) Acc
		(next sc:manglist (tail L) (+ Acc (list (sc:mang (head L))))))))

;; ---- top level -------------------------------------------------------
(define sc:lambda? (lambda (X)
	(if (= (typeof X) 'list)
		(if (empty? X) false (= (head X) 'lambda))
		false)))

(define sc:top1 (lambda (X)
	(if (= (typeof X) 'list)
		(if (empty? X) false
			(if (= (head X) 'define)
				(begin
				(set! sc:defined (+ sc:defined (list (second X))))
				(if (sc:lambda? (third X))
					(sc:fn (second X) (third X))
					(begin
						;; a top-level value: a global, initialised in order
						(set! sc:decls (+ sc:decls (+ "int *" (+ (sc:mang (second X)) (+ ";" NL)))))
						(set! sc:inits (+ sc:inits
							(+ TAB (+ "gc_add_root((int *)&"
								(+ (sc:mang (second X)) (+ ");" NL))))))
						(set! sc:curparams nil)
						(set! sc:curlocals nil)
						(set! sc:temp 0)
						(set! sc:maxtemp 0)
						(set! sc:body "")
						(sc:expr (third X) (sc:mang (second X)))
						(if (> sc:maxtemp sc:initmax) (set! sc:initmax sc:maxtemp) false)
						(set! sc:inits (+ sc:inits sc:body)))))
				(begin
					;; a bare top-level expression: run it in sc_init too
					(set! sc:temp 0)
					(set! sc:maxtemp 1)
					(set! sc:body "")
					(sc:expr X TopName)
					(if (> sc:maxtemp sc:initmax) (set! sc:initmax sc:maxtemp) false)
					(set! sc:inits (+ sc:inits sc:body)))))
		false)))

(define sc:top (lambda (L)
	(if (empty? L) false
		(begin (sc:top1 (head L)) (next sc:top (tail L))))))

;; ---- driver ----------------------------------------------------------
(define In (+ "" (head argv)))
(define Out (+ "" (head (tail argv))))
;; The init function is named per unit, because a host that links two
;; generated units would otherwise have two sc_init()s.
(define UnitName (if (empty? (tail (tail argv))) "" (+ "" (index argv 2))))
(define Unit (if (= UnitName "") "sc_init" (+ "sc_init_" UnitName)))
;; Literals and the top-level scratch are named PER UNIT. Two units in
;; one translation unit share a namespace -- including four generated
;; files into one host made lex's Q50 (the atom Id) and pp's Q50 (the
;; empty string) the same C global, and the later initialiser won, so
;; every identifier came out of the lexer with an empty kind. c4rlink
;; would have had the same collision across objects.
(set! sc:qpfx (if (= UnitName "") "Q" (+ "Q_" (+ UnitName "_"))))
(define TopName (if (= UnitName "") "sc_top" (+ "sc_top_" UnitName)))
(define Src (debug:parse (file:read In)))
(define Forms (if (= (head Src) 'begin) (tail Src) (list Src)))

(define sc:predefine (lambda (L)
	(if (empty? L) false
		(begin
			(if (= (typeof (head L)) 'list)
				(if (empty? (head L)) false
					(if (= (head (head L)) 'define)
						(set! sc:defined (+ sc:defined (list (second (head L))))) false))
				false)
			(sc:scan (head L))
			(next sc:predefine (tail L))))))

(sc:predefine Forms)
(sc:externs sc:called)
(sc:top Forms)

(define Text "")
(set! Text (+ "/* generated by c4sc from " (+ In " -- do not edit */")))
(set! Text (+ Text (+ NL NL)))
;; The runtime is DECLARED, not included. M1 measured what including it
;; costs: another ~3,100 preprocessed lines in every generated unit,
;; which is fine once and not fine twelve times. Declaring it makes each
;; unit stand alone, and c4lc -c turns every name below into an extern
;; symbol for c4rlink -- the same mechanism that already resolves the
;; functions a sibling .lisp defines. src/c4sc/scrt.h is the definition.
(define sc:proto (lambda (Sig) (set! Text (+ Text (+ Sig NL)))))
(set! Text (+ Text (+ "/* the runtime: src/c4sc/scrt.h defines these */" NL)))
(set! Text (+ Text (+ "extern int *cell_true;" NL)))
(set! Text (+ Text (+ "extern int *cell_false;" NL)))
(sc:proto "int *mk_int (int v);")
(sc:proto "int gc_add_root (int *slot);")
(sc:proto "int sc_true (int *x);")
(sc:proto "int *sc_cons (int *a, int *d);")
(sc:proto "int *sc_head (int *l);")
(sc:proto "int *sc_tail (int *l);")
(sc:proto "int *sc_empty (int *l);")
(sc:proto "int *sc_index (int *l, int *n);")
(sc:proto "int *sc_length (int *l);")
(sc:proto "int *sc_add (int *a, int *b);")
(sc:proto "int *sc_sub (int *a, int *b);")
(sc:proto "int *sc_mul (int *a, int *b);")
(sc:proto "int *sc_div (int *a, int *b);")
(sc:proto "int *sc_eq (int *a, int *b);")
(sc:proto "int *sc_ne (int *a, int *b);")
(sc:proto "int *sc_lt (int *a, int *b);")
(sc:proto "int *sc_le (int *a, int *b);")
(sc:proto "int *sc_gt (int *a, int *b);")
(sc:proto "int *sc_ge (int *a, int *b);")
(sc:proto "int *sc_not (int *a);")
(sc:proto "int *sc_bitand (int *a, int *b);")
(sc:proto "int *sc_bitor (int *a, int *b);")
(sc:proto "int *sc_bitxor (int *a, int *b);")
(sc:proto "int *sc_bitshl (int *a, int *b);")
(sc:proto "int *sc_bitshr (int *a, int *b);")
(sc:proto "int *sc_str_alloc (int *n);")
(sc:proto "int *sc_str_byte (int *s, int *i);")
(sc:proto "int *sc_str_setbyte (int *s, int *i, int *v);")
(sc:proto "int *sc_str_word (int *s, int *i);")
(sc:proto "int *sc_str_setword (int *s, int *i, int *v);")
(sc:proto "int *sc_str_substr (int *s, int *a, int *b);")
(sc:proto "int *sc_file_read (int *p);")
(sc:proto "int *sc_file_path (int *p);")
(sc:proto "int *sc_file_exists (int *p);")
(sc:proto "int *sc_error (int *args);")
(sc:proto "int *sc_typeof (int *x);")
(sc:proto "int *sc_print (int *args);")
(sc:proto "int *sc_str (char *s, int n);")
(sc:proto "int *sc_read (char *s, int n);")
(sc:proto "int *sc_atom (char *s, int n);")
(set! Text (+ Text NL))
(set! Text (+ Text (+ "int *" (+ TopName (+ ";" NL)))))
(set! Text (+ Text sc:extdecls))
(set! Text (+ Text sc:decls))
(set! Text (+ Text (+ NL (+ "void " (+ Unit (+ " ()" NL))))))
(set! Text (+ Text (+ "{" NL)))
;; sc_init is a function like any other and its temporaries have to be
;; declared: a top-level (define x (f (g y))) needs one, and C4 has no
;; mid-block declarations.
(set! Text (+ Text (sc:tempdecls 0 sc:initmax "")))
(set! Text (+ Text sc:inits))
(set! Text (+ Text (+ "}" (+ NL NL))))
(set! Text (+ Text sc:funs))

(file:write Out Text)
(print "c4sc:" In "->" Out (length Text) "bytes")

)
