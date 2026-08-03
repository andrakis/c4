;; c4lc-gen.lisp -- code generator for c4lc (docs/c4lc-design.md, L2)
;;
;; (gen:module AST) -> a c4r.lisp module list
;;   (version wordbits entry code data syms cons des dpatches)
;; ready for c4r:encode. Code is the labelled instruction-list form the
;; optimizer consumes; every function gets a label from a pre-pass, so
;; forward calls need none of c4cc's placeholder machinery.
;;
;; Emission mirrors c4cc's accumulator machine exactly: value in a,
;; PSH before binary ops, pointer arithmetic scaled by the word size,
;; SI/SC chosen by the lvalue's type, syscalls as opcode + ADJ n.
;; L2 scope: ints/chars/pointers, globals and locals with scalar
;; initializers (including "str" and &fn), all control flow (for
;; works -- it never did in c4cc), direct and function-pointer calls,
;; syscalls. Arrays, switch and variadics arrive in L3.

(begin

;; ---- helpers ----

(define g:reverse (lambda (l) (next g:reverse/2 l (list))))
(define g:reverse/2 (lambda (l acc)
	(if (empty? l) acc (next g:reverse/2 (tail l) (+ (list (head l)) acc)))))
(define g:cons (lambda (x l) (+ (list x) l)))
(define g:second (lambda (l) (index l 1)))
(define g:third (lambda (l) (index l 2)))

(define g:die (lambda (msg) (error (+ "c4lc gen: " msg))))

;; ---- machine constants (c4cc's) ----

(define g:CHAR 0)
(define g:INT 1)
(define g:PTR 2)
(define g:WORD 8)

;; ---- emission state ----

(define g:code nil)       ;; reversed instruction list
(define g:nlabel 0)       ;; next label id
(define g:ty 0)           ;; c4cc's ty: type of the value in the accumulator

(define g:emit (lambda (i) (set! g:code (g:cons i g:code))))
(define g:newlabel (lambda ()
	(begin
		(define l g:nlabel)
		(set! g:nlabel (+ l 1))
		l)))
(define g:label! (lambda (l) (g:emit (list 'label l))))

;; ---- data segment ----

(define g:DMAX 262144)
(define g:data nil)
(define g:dlen 0)

;; padding bytes must be written: the buffer is malloc'd, and garbage
;; padding would make the emitted image nondeterministic
(define g:dalign (lambda ()
	(if (= 0 (bit:and g:dlen 7)) nil
	(begin
		(string:byte! g:data g:dlen 0)
		(set! g:dlen (+ g:dlen 1))
		(next g:dalign)))))

;; append one word; returns its byte offset
(define g:dword (lambda (v)
	(begin
		(g:dalign)
		(define at g:dlen)
		(if (> (+ at g:WORD) g:DMAX) (g:die "data segment full") nil)
		(string:word! g:data at v)
		(set! g:dlen (+ at g:WORD))
		at)))

;; append string bytes + nul; returns the start offset
(define g:dstr (lambda (s)
	(begin
		(define at g:dlen)
		(if (> (+ at (length s) 1) g:DMAX) (g:die "data segment full") nil)
		(g:dstr/2 s 0 at)
		(set! g:dlen (+ at (length s) 1))
		at)))
(define g:dstr/2 (lambda (s i at)
	(if (>= i (length s)) (string:byte! g:data (+ at i) 0)
	(begin
		(string:byte! g:data (+ at i) (string:byte s i))
		(next g:dstr/2 s (+ i 1) at)))))

(define g:dpatches nil)   ;; (dcode BYTEOFF LABEL) / (ddata BYTEOFF VALOFF)

;; ---- symbol table ----
;; entries: (NAME 'sys TYPE OPATOM) | (NAME 'fun TYPE LABEL ARGC VARIADIC)
;;          (NAME 'glo TYPE BYTEOFF ARR) | (NAME 'loc TYPE IDX ARR)
;; ARR = total storage bytes for an array (the sizeof value), nil for
;; scalars. An array name evaluates to its ADDRESS (no load), c4cc's
;; ATTR_ARRAY semantics. Prepend order gives local-over-global
;; shadowing for free.

(define g:syms nil)
(define g:loc 0)          ;; c4cc's loc: LEA offset base inside a function
(define g:cons* nil)      ;; constructor labels (code L)
(define g:des* nil)       ;; destructor labels

(define g:lookup (lambda (n l)
	(if (empty? l) false
	(if (= (head (head l)) n) (head l)
	(next g:lookup n (tail l))))))
(define g:find (lambda (n)
	(begin
		(define s (g:lookup n g:syms))
		(if s s (g:die (+ "undefined identifier: " n))))))

;; c4cc's full library table: keyword order maps onto opcodes OPEN..FLT
(define g:syscalls '(
	("open" OPEN) ("read" READ) ("close" CLOS) ("printf" PRTF)
	("malloc" MALC) ("free" FREE) ("memset" MSET) ("memcmp" MCMP)
	("exit" EXIT) ("putchar" PUTC) ("puts" PUTS) ("realloc" RALC)
	("memcpy" MCPY) ("stacktrace" STRC)
	("install_trap_handler" ITH) ("__opcode" _OPC) ("__builtin" _BLT)
	("__c4_trap" _TRP) ("__c4_opcode" OPCD) ("__c4_jmp" _JMP)
	("__c4_adjust" _ADJ) ("__c4_configure" C4CF) ("__c4_cycles" C4CY)
	("__time" TIME) ("__c4_signal" SIGH) ("__c4_sigint" SIGI)
	("__c4_usleep" USLP) ("__c4_info" INFO) ("__c4_ops_list" OPSL)
	("__c4_invoke" C4IV) ("__c4_float" FLT)))
(define g:sysinit (lambda (l)
	(if (empty? l) nil
	(begin
		(set! g:syms (g:cons
			(list (head (head l)) 'sys g:INT (g:second (head l))) g:syms))
		(next g:sysinit (tail l))))))

;; ---- break/continue targets ----

(define g:brk nil)
(define g:cont nil)

;; ---- expressions ----

;; loads: LI/LC by type; stores: SI/SC
(define g:load! (lambda (ty)
	(g:emit (if (= ty g:CHAR) '(LC) '(LI)))))
(define g:store! (lambda (ty)
	(g:emit (if (= ty g:CHAR) '(SC) '(SI)))))

;; ---- struct types (L7) ----
;; encoding: 1024 + 64*id + 2*ptrlevel (see c4lc-parse.lisp)
(define g:STRUCT0 1024)
(define g:SSTEP 64)
(define g:structs nil)   ;; (ID SIZE MEMBERS), MEMBERS=((NAME OFF TYPE BYTES)...)

(define g:sid (lambda (ty) (/ (- ty g:STRUCT0) g:SSTEP)))
(define g:sptrlevel (lambda (ty)
	(- (- ty g:STRUCT0) (* g:SSTEP (g:sid ty)))))
;; struct VALUE type (pointer level 0)?
(define g:svalue? (lambda (ty)
	(if (< ty g:STRUCT0) false (= 0 (g:sptrlevel ty)))))
;; pointer of any flavour?
(define g:isptr (lambda (ty)
	(if (>= ty g:STRUCT0) (> (g:sptrlevel ty) 0) (>= ty g:PTR))))

(define g:sfind (lambda (id l)
	(if (empty? l) false
	(if (= (head (head l)) id) (head l)
	(next g:sfind id (tail l))))))
(define g:sinfo (lambda (ty)
	(begin
		(define s (g:sfind (g:sid ty) g:structs))
		(if s s (g:die "struct used before its definition")))))

;; storage size in bytes of a value of type ty
(define g:tysize (lambda (ty)
	(if (= ty g:CHAR) 1
	(if (< ty g:STRUCT0) g:WORD
	(if (g:svalue? ty) (g:second (g:sinfo ty))
	g:WORD)))))
;; size of what a pointer of type ty points at
(define g:elemsize (lambda (ty) (g:tysize (- ty g:PTR))))

;; member lookup: (NAME OFF TYPE BYTES) in struct-value type ty
(define g:member (lambda (ty n)
	(begin
		(if (g:svalue? ty) nil (g:die (+ "not a struct: ." n)))
		(define m (g:lookup n (g:third (g:sinfo ty))))
		(if m m (g:die (+ "no such member: " n))))))

;; struct/union layout from a (structdef NAME TYPE ISUNION MEMBERS)
;; node -- the parser bakes its assigned type id into the node, since
;; ids are handed out on FIRST MENTION (possibly a forward pointer
;; reference), not at definition. Every member starts word-aligned;
;; char arrays pack (n+7)/8 words.
(define g:structdef (lambda (d)
	(begin
		(define ty (g:third d))
		(define isu (index d 3))
		(define ms (g:slayout (index d 4) isu 0 0 (list)))
		(set! g:structs (g:cons
			(list (g:sid ty) (head ms) (g:reverse (g:second ms)))
			g:structs)))))
;; returns (SIZE REVMEMBERS)
(define g:slayout (lambda (ms isu off mx acc)
	(if (empty? ms)
		(list (if isu mx off) acc)
	(begin
		(define m (head ms))           ;; (TYPE NAME ASIZE)
		(define mty (head m))
		(define asz (g:third m))
		(define words
			(if (= asz nil)
				(/ (+ (g:tysize mty) 7) 8)
			(if (= (- mty g:PTR) g:CHAR)
				(/ (+ asz 7) 8)
			(* asz (/ (+ (g:elemsize mty) 7) 8)))))
		(define bytes
			(if (= asz nil)
				(if (g:svalue? mty) (g:tysize mty) nil)
			(if (= (- mty g:PTR) g:CHAR) asz (* asz (g:elemsize mty)))))
		(next g:slayout (tail ms) isu
			(if isu off (+ off (* words 8)))
			(if (> (* words 8) mx) (* words 8) mx)
			(g:cons (list (g:second m) off mty bytes) acc))))))

;; step size for ++/--/ptr arithmetic on type ty: pointers step their
;; element size (struct pointers step the struct size -- real C
;; semantics), scalars step 1, as c4cc
(define g:step (lambda (ty) (if (g:isptr ty) (g:elemsize ty) 1)))

;; c4cc's last_array: set when the last address came from an array
;; name (whose "address" IS its value: &arr is a no-op, assignment to
;; it is an error)
(define g:lastarray false)

;; address of an lvalue in the accumulator; g:ty = the VALUE's type
(define g:addr (lambda (e)
	(begin
		(define h (head e))
		(if (= h 'var) (g:varaddr (g:second e))
		(if (= h 'deref)
			(begin
				(g:expr (g:second e))
				(if (g:isptr g:ty) nil (g:die "bad dereference"))
				(set! g:ty (- g:ty g:PTR))
				(set! g:lastarray false))
		(if (= h 'index)
			(begin
				(g:indexaddr e)
				(set! g:lastarray false))
		(if (= h 'member) (g:memberaddr e false)
		(if (= h 'arrow) (g:memberaddr e true)
		(g:die (+ "bad lvalue: " (+ "" h)))))))))))

(define g:varaddr (lambda (n)
	(begin
		(define s (g:find n))
		(define cl (g:second s))
		(if (= cl 'loc) (g:emit (list 'LEA (- g:loc (index s 3))))
		(if (= cl 'glo) (g:emit (list 'IMM (list 'data (index s 3))))
		(if (if (= cl 'fun) true (= cl 'ext))
			;; &fn: the function's address. Marked like an array so
			;; assignment and ++/-- reject it as an lvalue.
			(g:emit (list 'IMM (g:funref s)))
		(g:die (+ "not a variable: " n)))))
		(set! g:ty (g:third s))
		(set! g:lastarray
			(if (= cl 'fun) true
			(if (= cl 'ext) true
				(if (= (index s 4) nil) false true)))))))

;; base[idx] address; g:ty = element type. Scaling is by the ELEMENT
;; size: 1 for char*, the struct size for struct pointers, 8 otherwise.
(define g:indexaddr (lambda (e)
	(begin
		(g:expr (g:second e))
		(define t g:ty)
		(if (g:isptr t) nil (g:die "pointer type expected"))
		(g:emit '(PSH))
		(g:expr (g:third e))
		(if (> (g:elemsize t) 1)
			(begin
				(g:emit '(PSH))
				(g:emit (list 'IMM (g:elemsize t)))
				(g:emit '(MUL)))
			nil)
		(g:emit '(ADD))
		(set! g:ty (- t g:PTR)))))

;; s.n / p->n: address of the member in the accumulator, g:ty = the
;; member's type. For . the operand is a struct VALUE expression
;; (which evaluates to its address, like arrays); for -> a pointer.
(define g:memberaddr (lambda (e viaptr)
	(begin
		(g:expr (g:second e))
		(define st (if viaptr (- g:ty g:PTR) g:ty))
		(define m (g:member st (g:third e)))
		(if (> (g:second m) 0)
			(begin
				(g:emit '(PSH))
				(g:emit (list 'IMM (g:second m)))
				(g:emit '(ADD)))
			nil)
		(set! g:ty (g:third m))
		;; array members (and struct-valued members) stay an address
		(set! g:lastarray
			(if (= (index m 3) nil) (g:svalue? (g:third m)) true)))))

;; ++x / --x (pre): address, load, adjust, store; value = new
(define g:preincdec (lambda (e op)
	(begin
		(g:addr (g:second e))
		(if g:lastarray (g:die "bad lvalue in pre-increment") nil)
		(define t g:ty)
		(g:emit '(PSH))
		(g:load! t)
		(g:emit '(PSH))
		(g:emit (list 'IMM (g:step t)))
		(g:emit (list op))
		(g:store! t)
		(set! g:ty t))))

;; x++ / x-- (post): as pre, then undo the adjustment in the result
(define g:postincdec (lambda (e op unop)
	(begin
		(g:preincdec e op)
		(define t g:ty)
		(g:emit '(PSH))
		(g:emit (list 'IMM (g:step t)))
		(g:emit (list unop))
		(set! g:ty t))))

;; binary operator with c4cc's pointer scaling
(define g:binary (lambda (e opname)
	(begin
		(g:expr (g:second e))
		(define t g:ty)
		(define es (if (g:isptr t) (g:elemsize t) 1))
		(g:emit '(PSH))
		(g:expr (g:third e))
		(if (= opname 'ADD)
			(begin
				(if (> es 1)
					(begin
						(g:emit '(PSH))
						(g:emit (list 'IMM es))
						(g:emit '(MUL)))
					nil)
				(g:emit '(ADD))
				(set! g:ty t))
		(if (= opname 'SUB)
			(if (if (if (g:isptr t) (> es 1) false) (= t g:ty) false)
				(begin        ;; ptr - ptr: difference in elements
					(g:emit '(SUB))
					(g:emit '(PSH))
					(g:emit (list 'IMM es))
					(g:emit '(DIV))
					(set! g:ty g:INT))
			(begin
				(if (> es 1)
					(begin
						(g:emit '(PSH))
						(g:emit (list 'IMM es))
						(g:emit '(MUL)))
					nil)
				(g:emit '(SUB))
				(set! g:ty t)))
		(begin
			(g:emit (list opname))
			(set! g:ty g:INT)))))))

(define g:binops '(
	(bor OR) (bxor XOR) (band AND) (eq EQ) (ne NE) (lt LT) (gt GT)
	(le LE) (ge GE) (shl SHL) (shr SHR) (add ADD) (sub SUB)
	(mul MUL) (div DIV) (mod MOD)))
(define g:assoc/atom (lambda (k l)
	(if (empty? l) false
	(if (= (head (head l)) k) (head l)
	(next g:assoc/atom k (tail l))))))

;; call: args left to right, each pushed; then Sys opcode / JSR / JSRI /
;; JSRS; then ADJ n
;; the JSR target operand for a fun or ext entry: in-unit label, or an
;; extern placeholder rewritten to a symbol reference at assembly
(define g:funref (lambda (s)
	(if (= (g:second s) 'ext)
		(list 'extph (index s 3))
	(list 'code (index s 3)))))

(define g:call (lambda (e)
	(begin
		(define n (g:second e))
		(define t (g:pushargs (tail (tail e)) 0))
		(define s (g:find n))
		(define cl (g:second s))
		(if (= cl 'sys) (g:emit (list (index s 3)))
		(if (if (= cl 'fun) true (= cl 'ext))
			(begin
				(if (index s 5)
					;; variadic: bundle the extra args through
					;; __c4cc_make_va, exactly as c4cc (the va pointer
					;; becomes the final argument, filling the fake slot)
					(begin
						(define argc (index s 4))
						(define mv (g:find "__c4cc_make_va"))
						(if (if (= (g:second mv) 'fun) true (= (g:second mv) 'ext)) nil
							(g:die "vararg support requires __c4cc_make_va, include stdarg.h"))
						(g:emit (list 'IMM (+ (- t argc) 1)))
						(g:emit '(PSH))
						(g:emit (list 'JSR (g:funref mv)))
						(g:emit (list 'ADJ (+ (- t argc) 2)))
						(g:emit '(PSH))
						(set! t argc))
				(if (= t (index s 4)) nil
					(print ";; c4lc WARNING: argument count mismatch in call to"
						n "- expected" (index s 4) "given" t)))
				(g:emit (list 'JSR (g:funref s))))
		(if (= cl 'glo) (g:emit (list 'JSRI (list 'data (index s 3))))
		(if (= cl 'loc) (g:emit (list 'JSRS (- g:loc (index s 3))))
		(g:die (+ "bad function call: " n))))))
		(if (> t 0) (g:emit (list 'ADJ t)) nil)
		(set! g:ty (g:third s)))))
(define g:pushargs (lambda (args t)
	(if (empty? args) t
	(begin
		(g:expr (head args))
		(g:emit '(PSH))
		(next g:pushargs (tail args) (+ t 1))))))

(define g:comma (lambda (es)
	(if (empty? (tail es)) (g:expr (head es))
	(begin
		(g:expr (head es))
		(next g:comma (tail es))))))

(define g:expr (lambda (e)
	(begin
		(define h (head e))
		(if (= h 'num)
			(begin
				(g:emit (list 'IMM (g:second e)))
				(set! g:ty g:INT))
		(if (= h 'str)
			(begin
				(g:emit (list 'IMM (list 'data (g:dstr (g:second e)))))
				(set! g:ty g:PTR))
		(if (= h 'var) (g:varexpr (g:second e))
		(if (= h 'call) (g:call e)
		(if (= h 'assign)
			(begin
				(g:addr (g:second e))
				(if g:lastarray (g:die "bad lvalue in assignment") nil)
				(define t g:ty)
				(g:emit '(PSH))
				(g:expr (g:third e))
				(g:store! t)
				(set! g:ty t))
		(if (= h 'deref)
			(begin
				(g:expr (g:second e))
				(if (g:isptr g:ty) nil (g:die "bad dereference"))
				(set! g:ty (- g:ty g:PTR))
				(if (g:svalue? g:ty)
					(set! g:lastarray true)   ;; *structptr is an address
				(g:load! g:ty)))
		(if (= h 'addr)
			(begin
				(g:addr (g:second e))
				(set! g:ty (+ g:ty g:PTR)))
		(if (= h 'lognot)
			(begin
				(g:expr (g:second e))
				(g:emit '(PSH))
				(g:emit '(IMM 0))
				(g:emit '(EQ))
				(set! g:ty g:INT))
		(if (= h 'bitnot)
			(begin
				(g:expr (g:second e))
				(g:emit '(PSH))
				(g:emit '(IMM -1))
				(g:emit '(XOR))
				(set! g:ty g:INT))
		(if (= h 'neg)
			(begin
				(g:emit '(IMM -1))
				(g:emit '(PSH))
				(g:expr (g:second e))
				(g:emit '(MUL))
				(set! g:ty g:INT))
		(if (= h 'preinc) (g:preincdec e 'ADD)
		(if (= h 'predec) (g:preincdec e 'SUB)
		(if (= h 'postinc) (g:postincdec e 'ADD 'SUB)
		(if (= h 'postdec) (g:postincdec e 'SUB 'ADD)
		(if (= h 'cast)
			(begin
				(g:expr (g:third e))
				(set! g:ty (g:second e)))
		(if (= h 'sizeof)
			(begin
				(g:emit (list 'IMM (g:tysize (g:second e))))
				(set! g:ty g:INT))
		(if (= h 'sizeofa)
			(begin
				(define sa (g:find (g:second e)))
				(if (= (index sa 4) nil)
					(g:die (+ "sizeof needs an array: " (g:second e))) nil)
				(g:emit (list 'IMM (index sa 4)))
				(set! g:ty g:INT))
		(if (= h 'index)
			(begin
				(g:indexaddr e)
				(if (g:svalue? g:ty)
					(set! g:lastarray true)   ;; struct element: address
				(g:load! g:ty)))
		(if (= h 'member)
			(begin
				(g:memberaddr e false)
				(if g:lastarray nil (g:load! g:ty)))
		(if (= h 'arrow)
			(begin
				(g:memberaddr e true)
				(if g:lastarray nil (g:load! g:ty)))
		(if (= h 'cond) (g:condexpr e)
		(if (= h 'land)
			(begin
				(g:expr (g:second e))
				(define l (g:newlabel))
				(g:emit (list 'BZ (list 'code l)))
				(g:expr (g:third e))
				(g:label! l)
				(set! g:ty g:INT))
		(if (= h 'lor)
			(begin
				(g:expr (g:second e))
				(define l2 (g:newlabel))
				(g:emit (list 'BNZ (list 'code l2)))
				(g:expr (g:third e))
				(g:label! l2)
				(set! g:ty g:INT))
		(if (= h 'comma) (g:comma (tail e))
		(begin
			(define b (g:assoc/atom h g:binops))
			(if b (g:binary e (g:second b))
			(g:die (+ "bad expression node: " (+ "" h))))))))))))))))))))))))))))))))

(define g:varexpr (lambda (n)
	(begin
		(define s (g:find n))
		(define cl (g:second s))
		(if (= cl 'loc)
			(begin
				(g:emit (list 'LEA (- g:loc (index s 3))))
				(g:varload s))
		(if (= cl 'glo)
			(begin
				(g:emit (list 'IMM (list 'data (index s 3))))
				(g:varload s))
		(if (if (= cl 'fun) true (= cl 'ext))
			(begin
				;; function name as a value: its address
				(g:emit (list 'IMM (g:funref s)))
				(set! g:ty (g:third s))
				(set! g:lastarray false))
		(g:die (+ "bad variable: " n))))))))

;; after the address is in the accumulator: arrays stay an address,
;; scalars load their value
(define g:varload (lambda (s)
	(begin
		(set! g:ty (g:third s))
		(if (= (index s 4) nil)
			(begin
				(set! g:lastarray false)
				(g:load! g:ty))
		(set! g:lastarray true)))))

(define g:condexpr (lambda (e)
	(begin
		(g:expr (g:second e))
		(define l1 (g:newlabel))
		(define l2 (g:newlabel))
		(g:emit (list 'BZ (list 'code l1)))
		(g:expr (g:third e))
		(g:emit (list 'JMP (list 'code l2)))
		(g:label! l1)
		(g:expr (index e 3))
		(g:label! l2))))

;; ---- statements ----

;; frame words needed by declstmt nodes anywhere under a statement
;; (ENT must reserve the whole frame up front)
(define g:scanstmt (lambda (s)
	(begin
		(define h (head s))
		(if (= h 'declstmt) (g:scandecls (tail s) 0)
		(if (= h 'block) (g:scanlist (tail s) 0)
		(if (= h 'if)
			(+ (g:scanstmt (g:third s))
				(if (= (index s 3) nil) 0 (g:scanstmt (index s 3))))
		(if (= h 'while) (g:scanstmt (g:third s))
		(if (= h 'dowhile) (g:scanstmt (g:second s))
		(if (= h 'for) (g:scanstmt (index s 4))
		(if (= h 'switch) (g:scanlist (tail (tail s)) 0)
		0))))))))))
(define g:scanlist (lambda (l n)
	(if (empty? l) n
	(next g:scanlist (tail l) (+ n (g:scanstmt (head l)))))))
(define g:scandecls (lambda (ls n)
	(if (empty? ls) n
	(next g:scandecls (tail ls) (+ n (g:lwords (head ls)))))))

(define g:lcur 0)   ;; frame slot cursor for block-scoped declarations

(define g:stmt (lambda (s)
	(begin
		(define h (head s))
		(if (= h 'block)
			(begin
				;; block scope: declarations inside vanish at the brace
				(define scope g:syms)
				(g:stmts (tail s))
				(set! g:syms scope))
		(if (= h 'expr) (g:expr (g:second s))
		(if (= h 'if) (g:ifstmt s)
		(if (= h 'while) (g:whilestmt s)
		(if (= h 'dowhile) (g:dowhilestmt s)
		(if (= h 'declstmt) (g:declstmts (tail s))
		(if (= h 'for) (g:forstmt s)
		(if (= h 'return)
			(begin
				(if (= (g:second s) nil) nil (g:expr (g:second s)))
				(g:emit '(LEV)))
		(if (= h 'break)
			(if (= g:brk nil) (g:die "break outside of loop or switch")
				(g:emit (list 'JMP (list 'code g:brk))))
		(if (= h 'continue)
			(if (= g:cont nil) (g:die "continue outside of loop")
				(g:emit (list 'JMP (list 'code g:cont))))
		(if (= h 'empty) nil
		(if (= h 'switch) (g:switchstmt s)
		(g:die (+ "bad statement node: " (+ "" h))))))))))))))))))
(define g:stmts (lambda (l)
	(if (empty? l) nil
	(begin
		(g:stmt (head l))
		(next g:stmts (tail l))))))

(define g:ifstmt (lambda (s)
	(begin
		(g:expr (g:second s))
		(define l1 (g:newlabel))
		(g:emit (list 'BZ (list 'code l1)))
		(g:stmt (g:third s))
		(if (= (index s 3) nil)
			(g:label! l1)
		(begin
			(define l2 (g:newlabel))
			(g:emit (list 'JMP (list 'code l2)))
			(g:label! l1)
			(g:stmt (index s 3))
			(g:label! l2))))))

(define g:whilestmt (lambda (s)
	(begin
		(define lc (g:newlabel))
		(define lend (g:newlabel))
		(define ob g:brk)
		(define oc g:cont)
		(set! g:brk lend)
		(set! g:cont lc)
		(g:label! lc)
		(g:expr (g:second s))
		(g:emit (list 'BZ (list 'code lend)))
		(g:stmt (g:third s))
		(g:emit (list 'JMP (list 'code lc)))
		(g:label! lend)
		(set! g:brk ob)
		(set! g:cont oc))))

;; for (init; cond; step) body -- with an empty cond the loop is
;; unconditional; continue lands on the step
(define g:forstmt (lambda (s)
	(begin
		(define lc (g:newlabel))
		(define lstep (g:newlabel))
		(define lend (g:newlabel))
		(define ob g:brk)
		(define oc g:cont)
		(set! g:brk lend)
		(set! g:cont lstep)
		(if (= (g:second s) nil) nil (g:expr (g:second s)))
		(g:label! lc)
		(if (= (g:third s) nil) nil
		(begin
			(g:expr (g:third s))
			(g:emit (list 'BZ (list 'code lend)))))
		(g:stmt (index s 4))
		(g:label! lstep)
		(if (= (index s 3) nil) nil (g:expr (index s 3)))
		(g:emit (list 'JMP (list 'code lc)))
		(g:label! lend)
		(set! g:brk ob)
		(set! g:cont oc))))

;; ---- switch: c4cc's data-segment jumptable, with labels ----
;;
;;     <expr>                a = value
;;     JMP dispatch
;;   body:                   (case V) marks a label; break -> end
;;     JMP end               (running off the end of the body)
;;   dispatch:
;;     [PSH; IMM min; SUB]   a = idx = value - min    (when min != 0)
;;     PSH; PSH; PSH         three idx copies on the stack
;;     IMM range; GT; BNZ oob2
;;     IMM 0;     LT; BNZ oob1
;;     IMM 8; MUL; PSH; IMM table; ADD; LI; JMPA
;;   oob2: ADJ 2; JMP default-or-end
;;   oob1: ADJ 1; JMP default-or-end
;;   end:
;;
;; The table words are zero in the pool; each is a (dcode OFF LABEL)
;; patch the loader resolves. JMPA needs c4m at runtime, as in c4cc.

(define g:swcases nil)    ;; ((V LABEL) ...) of the innermost switch
(define g:swdef nil)      ;; its default label, or nil
(define g:SWRANGE 4096)

(define g:swfind (lambda (v l)
	(if (empty? l) false
	(if (= (head (head l)) v) (head l)
	(next g:swfind v (tail l))))))
(define g:swmin (lambda (l m)
	(if (empty? l) m
	(next g:swmin (tail l) (if (< (head (head l)) m) (head (head l)) m)))))
(define g:swmax (lambda (l m)
	(if (empty? l) m
	(next g:swmax (tail l) (if (> (head (head l)) m) (head (head l)) m)))))

(define g:switchbody (lambda (items)
	(if (empty? items) nil
	(begin
		(define it (head items))
		(if (= (head it) 'case)
			(begin
				(define v (g:second it))
				(if (g:swfind v g:swcases)
					(g:die (+ "duplicate case value " v)) nil)
				(define lc (g:newlabel))
				(g:label! lc)
				(set! g:swcases (g:cons (list v lc) g:swcases)))
		(if (= (head it) 'default)
			(begin
				(if (= g:swdef nil) nil (g:die "duplicate default"))
				(define ld (g:newlabel))
				(g:label! ld)
				(set! g:swdef ld))
		(g:stmt it)))
		(next g:switchbody (tail items))))))

;; table words + their dcode patches; unmatched slots go to default/end
(define g:swtable (lambda (cases def lend mn i range tbl)
	(if (> i range) nil
	(begin
		(g:dword 0)
		(define c (g:swfind (+ mn i) cases))
		(define target (if c (g:second c) (if (= def nil) lend def)))
		(set! g:dpatches (g:cons
			(list 'dcode (+ tbl (* i g:WORD)) target) g:dpatches))
		(next g:swtable cases def lend mn (+ i 1) range tbl)))))

(define g:swdispatch (lambda (cases def lend)
	(if (empty? cases)
		;; no cases at all: default if present, else fall through to end
		(if (= def nil) nil (g:emit (list 'JMP (list 'code def))))
	(begin
		(define mn (g:swmin cases (head (head cases))))
		(define range (- (g:swmax cases (head (head cases))) mn))
		(if (>= range g:SWRANGE)
			(g:die (+ "switch range " range " too sparse for a jump table")) nil)
		(g:dalign)
		(define tbl g:dlen)
		(g:swtable cases def lend mn 0 range tbl)
		(define defloc (if (= def nil) lend def))
		(define lo2 (g:newlabel))
		(define lo1 (g:newlabel))
		(if (= mn 0) nil
			(begin
				(g:emit '(PSH))
				(g:emit (list 'IMM mn))
				(g:emit '(SUB))))
		(g:emit '(PSH))
		(g:emit '(PSH))
		(g:emit '(PSH))
		(g:emit (list 'IMM range))
		(g:emit '(GT))
		(g:emit (list 'BNZ (list 'code lo2)))
		(g:emit '(IMM 0))
		(g:emit '(LT))
		(g:emit (list 'BNZ (list 'code lo1)))
		(g:emit (list 'IMM g:WORD))
		(g:emit '(MUL))
		(g:emit '(PSH))
		(g:emit (list 'IMM (list 'data tbl)))
		(g:emit '(ADD))
		(g:emit '(LI))
		(g:emit '(JMPA))
		(g:label! lo2)
		(g:emit '(ADJ 2))
		(g:emit (list 'JMP (list 'code defloc)))
		(g:label! lo1)
		(g:emit '(ADJ 1))
		(g:emit (list 'JMP (list 'code defloc)))))))

(define g:switchstmt (lambda (s)
	(begin
		(g:expr (g:second s))
		(define ld (g:newlabel))
		(define lend (g:newlabel))
		(define ob g:brk)
		(define ocases g:swcases)
		(define odef g:swdef)
		(set! g:brk lend)
		(set! g:swcases (list))
		(set! g:swdef nil)
		(g:emit (list 'JMP (list 'code ld)))
		(g:switchbody (tail (tail s)))
		(define cases (g:reverse g:swcases))
		(define def g:swdef)
		(set! g:brk ob)
		(set! g:swcases ocases)
		(set! g:swdef odef)
		(g:emit (list 'JMP (list 'code lend)))
		(g:label! ld)
		(g:swdispatch cases def lend)
		(g:label! lend))))

;; ---- declarations ----

;; pre-pass: register every defined function so forward calls resolve
(define g:prepass (lambda (decls)
	(if (empty? decls) nil
	(begin
		(define d (head decls))
		(if (= (head d) 'func)
			(set! g:syms (g:cons
				(list (g:third d) 'fun (g:second d) (g:newlabel)
					;; ArgCount includes the variadic fake slot, as c4cc
					(+ (length (index d 3)) (if (index d 4) 1 0))
					(index d 4)
					(index d 5))   ;; declaration attrs (static etc.)
				g:syms))
			nil)
		(next g:prepass (tail decls))))))

;; second pre-pass: prototypes with no definition anywhere in the
;; unit. In OBJECT MODE (-c) they become extern symbols: calls carry
;; SYMBOL-typed patches for c4rlink to resolve. In whole-program mode
;; they get a stub that exits with 255 if ever reached -- c4cc
;; "supports" these by emitting a call through an unresolved extern
;; (which jumps to garbage); the stub is the deterministic version of
;; the same contract: legal to declare and call-compile, fatal to
;; execute.
(define gen:objmode false)
(define g:stubs nil)     ;; (LABEL ...) pending stub labels
(define g:externs nil)   ;; (NAME TYPE VARIADIC) in first-seen order
(define g:nexterns 0)
(define g:protopass (lambda (decls)
	(if (empty? decls) nil
	(begin
		(define d (head decls))
		(if (if (= (head d) 'proto)
				(= (g:lookup (g:third d) g:syms) false)
				false)
			(if gen:objmode
				(begin
					(set! g:syms (g:cons
						(list (g:third d) 'ext (g:second d) g:nexterns
							(+ (length (index d 3)) (if (index d 4) 1 0))
							(index d 4))
						g:syms))
					(set! g:externs (g:cons
						(list (g:third d) (g:second d) (index d 4))
						g:externs))
					(set! g:nexterns (+ g:nexterns 1)))
			(begin
				(define l (g:newlabel))
				(set! g:syms (g:cons
					(list (g:third d) 'fun (g:second d) l
						(+ (length (index d 3)) (if (index d 4) 1 0))
						(index d 4)
						16)   ;; ATTR_EXTERN, as c4cc marks prototypes
					g:syms))
				(set! g:stubs (g:cons l g:stubs))))
			nil)
		(next g:protopass (tail decls))))))
(define g:emitstubs (lambda (ls)
	(if (empty? ls) nil
	(begin
		(g:label! (head ls))
		(g:emit '(ENT 0))
		(g:emit '(IMM 255))
		(g:emit '(PSH))
		(g:emit '(EXIT))
		(next g:emitstubs (tail ls))))))

;; write N zero bytes at the current data cursor
(define g:dzero (lambda (nb)
	(if (= nb 0) nil
	(begin
		(if (>= g:dlen g:DMAX) (g:die "data segment full") nil)
		(string:byte! g:data g:dlen 0)
		(set! g:dlen (+ g:dlen 1))
		(next g:dzero (- nb 1))))))

;; write brace-list values as words or bytes, zero-filling to s elements
(define g:delems (lambda (vs s bytewise)
	(if (= s 0) nil
	(begin
		(define v (if (empty? vs) 0 (head vs)))
		(if bytewise
			(begin
				(string:byte! g:data g:dlen v)
				(set! g:dlen (+ g:dlen 1)))
			(g:dword v))
		(next g:delems (if (empty? vs) vs (tail vs)) (- s 1) bytewise)))))

;; global declaration: scalar word (c4cc gives char globals a whole
;; word too) or array storage; "str" and &fn initializers add
;; data-resident patches
(define g:global (lambda (d)
	(begin
		(define ty (g:second d))
		(define n (g:third d))
		(define size (index d 3))
		(define init (index d 5))
		(define gattrs (index d 4))
		(if (if (= size nil) (g:svalue? ty) false)
			(g:globalstruct n ty init gattrs)
		(if (= size nil)
			(g:globalscalar n ty init gattrs)
		(g:globalarray n ty size init gattrs))))))

;; struct-valued global: zeroed storage, name evaluates to its address
(define g:globalstruct (lambda (n ty init gattrs)
	(begin
		(if (= init nil) nil
			(g:die (+ "struct globals cannot be initialized: " n)))
		(g:dalign)
		(define at g:dlen)
		(define bytes (g:tysize ty))
		(if (> (+ at bytes) g:DMAX) (g:die "data segment full") nil)
		(g:dzero bytes)
		(set! g:syms (g:cons (list n 'glo ty at bytes gattrs) g:syms)))))

(define g:globalscalar (lambda (n ty init gattrs)
	(begin
		(define at
			(if (= init nil) (g:dword 0)
			(if (= (head init) 'num) (g:dword (g:second init))
			(if (= (head init) 'str)
				(begin
					(define soff (g:dstr (g:second init)))
					(define slot (g:dword 0))
					(set! g:dpatches (g:cons (list 'ddata slot soff) g:dpatches))
					slot)
			(if (= (head init) 'fnaddr)
				(begin
					(define f (g:find (g:second init)))
					(if (= (g:second f) 'fun) nil
						(g:die "& initializer requires a defined function"))
					(define slot (g:dword 0))
					(set! g:dpatches (g:cons
						(list 'dcode slot (index f 3)) g:dpatches))
					slot)
			(g:die (+ "unsupported global initializer for " n)))))))
		(set! g:syms (g:cons (list n 'glo ty at nil gattrs) g:syms)))))

;; array storage: char arrays are s bytes, everything else s words;
;; uninitialized/short-initialized elements are zero
(define g:globalarray (lambda (n ty s init gattrs)
	(begin
		(define elem (- ty g:PTR))
		(define bytewise (= elem g:CHAR))
		(g:dalign)
		(define at g:dlen)
		(define bytes (if bytewise s (* s (* 8 (/ (+ (g:tysize elem) 7) 8)))))
		(if (> (+ at bytes) g:DMAX) (g:die "data segment full") nil)
		(if (= init nil) (g:dzero bytes)
		(if (= (head init) 'braces)
			(begin
				(if (g:svalue? elem)
					(g:die (+ "struct arrays cannot be initialized: " n)) nil)
				(if (> (length (g:second init)) s)
					(g:die (+ "too many initializers for " n)) nil)
				(g:delems (g:second init) s bytewise))
		(if (if (= (head init) 'str) bytewise false)
			(begin
				(if (< s (+ (length (g:second init)) 1))
					(g:die (+ "string does not fit the array " n)) nil)
				(g:dstrbytes (g:second init) 0)
				(g:dzero (- s (length (g:second init)))))
		(g:die (+ "unsupported array initializer for " n)))))
		(set! g:syms (g:cons (list n 'glo ty at bytes gattrs) g:syms)))))
(define g:dstrbytes (lambda (str i)
	(if (>= i (length str)) nil
	(begin
		(string:byte! g:data g:dlen (string:byte str i))
		(set! g:dlen (+ g:dlen 1))
		(next g:dstrbytes str (+ i 1))))))

;; locals: params idx 0..argc-1, loc = argc+1, scalars idx loc+1...;
;; initializer stores re-run on every entry, emitted after ENT
(define g:params (lambda (ps i)
	(if (empty? ps) i
	(begin
		(set! g:syms (g:cons
			(list (g:second (head ps)) 'loc (head (head ps)) i nil) g:syms))
		(next g:params (tail ps) (+ i 1))))))

;; local slots, c4cc's exact scheme: i += words consumed, the symbol's
;; Val is i AFTER the add, so LEA (loc - Val) is the LOWEST slot and
;; array indexing ascends. char arrays pack bytes into whole words.
;; frame words one declaration needs (struct-aware)
(define g:lwords (lambda (d)
	(begin
		(define size (index d 3))
		(define ty (g:second d))
		(if (= size nil)
			(if (g:svalue? ty) (/ (+ (g:tysize ty) 7) 8) 1)
		(if (= (- ty g:PTR) g:CHAR)
			(/ (+ size 7) 8)
		(* size (/ (+ (g:elemsize ty) 7) 8)))))))
;; its sizeof value in bytes, nil for plain scalars
(define g:lbytes (lambda (d)
	(begin
		(define size (index d 3))
		(define ty (g:second d))
		(if (= size nil)
			(if (g:svalue? ty) (g:tysize ty) nil)
		(if (= (- ty g:PTR) g:CHAR) size (* size (g:elemsize ty)))))))

(define g:localdefs (lambda (ls i)
	(if (empty? ls) i
	(begin
		(define d (head ls))   ;; (local TYPE NAME SIZE INIT)
		(define words (g:lwords d))
		(set! g:syms (g:cons
			(list (g:third d) 'loc (g:second d) (+ i words) (g:lbytes d))
			g:syms))
		(next g:localdefs (tail ls) (+ i words))))))

;; initializer stores, emitted after ENT so they re-run per entry.
;; Word stores use LEA's constant offset; byte stores add the index at
;; run time since LEA only reaches word slots. Elements past the
;; initializer zero-fill, as C requires.
(define g:linit/word (lambda (val idx v)
	(begin
		(g:emit (list 'LEA (+ (- g:loc val) idx)))
		(g:emit '(PSH))
		(if (= 'list (typeof v)) (g:emit v) (g:emit (list 'IMM v)))
		(g:emit '(SI)))))
(define g:linit/byte (lambda (val idx v)
	(begin
		(g:emit (list 'LEA (- g:loc val)))
		(g:emit '(PSH))
		(g:emit (list 'IMM idx))
		(g:emit '(ADD))
		(g:emit '(PSH))
		(g:emit (list 'IMM v))
		(g:emit '(SC)))))

(define g:linit/elems (lambda (val vs s idx bytewise)
	(if (>= idx s) nil
	(begin
		(define v (if (empty? vs) 0 (head vs)))
		(if bytewise (g:linit/byte val idx v) (g:linit/word val idx v))
		(next g:linit/elems val (if (empty? vs) vs (tail vs)) s (+ idx 1) bytewise)))))

(define g:strbytes (lambda (str i acc)   ;; string -> list of byte values
	(if (>= i (length str)) (g:reverse acc)
	(next g:strbytes str (+ i 1) (g:cons (string:byte str i) acc)))))

;; emit the initializer of local node d whose frame slot is val
(define g:eminit (lambda (d val)
	(begin
		(define init (index d 4))
		(define size (index d 3))
		(define bytewise
			(if (= size nil) false
				(= (- (g:second d) g:PTR) g:CHAR)))
		(if (= init nil) nil
		(if (= size nil)
			;; scalar: a full initializer expression, stored as a
			;; word (c4cc used SI even for char scalars)
			(if (= (head init) 'einit)
				(begin
					(if (g:svalue? (g:second d))
						(g:die "cannot initialize a struct by value") nil)
					(g:emit (list 'LEA (- g:loc val)))
					(g:emit '(PSH))
					(g:expr (g:second init))
					(g:emit '(SI)))
			(g:die "unsupported local initializer"))
		(if (= (head init) 'braces)
			(begin
				(if (> (length (g:second init)) size)
					(g:die (+ "too many initializers for " (g:third d))) nil)
				(g:linit/elems val (g:second init) size 0 bytewise))
		(if (if (= (head init) 'str) bytewise false)
			(begin
				(if (< size (+ (length (g:second init)) 1))
					(g:die (+ "string does not fit the array " (g:third d))) nil)
				(g:linit/elems val (g:strbytes (g:second init) 0 (list))
					size 0 true))
		(g:die (+ "unsupported array initializer for " (g:third d))))))))))

(define g:localinits (lambda (ls)
	(if (empty? ls) nil
	(begin
		(define d (head ls))
		(if (= (index d 4) nil) nil
			(g:eminit d (index (g:find (g:third d)) 3)))
		(next g:localinits (tail ls))))))

;; block-position declarations: allocate from the frame cursor,
;; register (block scope handles removal), run the initializer HERE --
;; a block declaration initializes when reached, as C requires
(define g:declstmts (lambda (ls)
	(if (empty? ls) nil
	(begin
		(define d (head ls))
		(set! g:lcur (+ g:lcur (g:lwords d)))
		(set! g:syms (g:cons
			(list (g:third d) 'loc (g:second d) g:lcur (g:lbytes d))
			g:syms))
		(g:eminit d g:lcur)
		(next g:declstmts (tail ls))))))

(define g:dowhilestmt (lambda (s)
	(begin
		(define lb (g:newlabel))
		(define lc (g:newlabel))
		(define lend (g:newlabel))
		(define ob g:brk)
		(define oc g:cont)
		(set! g:brk lend)
		(set! g:cont lc)
		(g:label! lb)
		(g:stmt (g:second s))
		(g:label! lc)
		(g:expr (g:third s))
		(g:emit (list 'BNZ (list 'code lb)))
		(g:label! lend)
		(set! g:brk ob)
		(set! g:cont oc))))

(define g:function (lambda (d)
	(begin
		(define n (g:third d))
		(define ps (index d 3))
		(define attrs (index d 5))
		(define ls (tail (index d 6)))
		(define body (index d 7))
		(define s (g:lookup n g:syms))
		(define oldsyms g:syms)
		;; a variadic's ... occupies one unnamed slot (c4cc's fake
		;; parameter), counted in argc so va_start's &LAST - 1 lands on it
		(define argc (+ (g:params ps 0) (if (index d 4) 1 0)))
		(set! g:loc (+ argc 1))
		(define fini (g:localdefs ls (+ argc 1)))
		;; block-scoped declarations deeper in the body still need
		;; their frame words reserved by ENT; a pre-scan counts them
		(define extra (g:scanstmt body))
		(set! g:lcur fini)
		(g:label! (index s 3))
		(g:emit (list 'ENT (+ (- fini g:loc) extra)))
		(g:localinits ls)
		(g:stmts (tail body))
		(if (= (head (head g:code)) 'LEV) nil (g:emit '(LEV)))
		(set! g:syms oldsyms)
		(if (= 1 (bit:and attrs 1))
			(set! g:cons* (g:cons (list 'code (index s 3)) g:cons*)) nil)
		(if (= 2 (bit:and attrs 2))
			(set! g:des* (g:cons (list 'code (index s 3)) g:des*)) nil))))

(define g:decl (lambda (d)
	(begin
		(define h (head d))
		(if (= h 'enum) nil            ;; constants already substituted
		(if (= h 'proto) nil           ;; pre-pass covers in-unit targets
		(if (= h 'typedefd) nil        ;; parser-resolved
		(if (= h 'structdef) (g:structdef d)
		(if (= h 'global) (g:global d)
		(if (= h 'func) (g:function d)
		(g:die (+ "bad declaration node: " (+ "" h))))))))))))
(define g:decls (lambda (l)
	(if (empty? l) nil
	(begin
		(g:decl (head l))
		(next g:decls (tail l))))))

;; ---- symbol section ----
;; c4cc numeric classes: Num=128 Fun=129 Sys=130 Glo=131 Loc=132.
;; Functions carry their code label as the value (the encoder resolves
;; it to the word offset); load-c4r's stacktrace walks exactly these.
;; Globals' values are their data offsets. Attrs: variadic 0x20,
;; array 0x40.
(define g:symsection (lambda (entries id acc)
	(if (empty? entries) (g:reverse acc)   ;; call with declaration order
	(begin
		(define s (head entries))
		(define cl (g:second s))
		(next g:symsection (tail entries)
			(if (= cl 'fun) (+ id 1) (if (= cl 'glo) (+ id 1) id))
			(if (= cl 'fun)
				(g:cons (list id (g:third s) 129
					(bit:or (if (= (index s 6) nil) 0 (index s 6))
						(if (index s 5) 32 0))
					(head s) (list 'code (index s 3))) acc)
			(if (= cl 'glo)
				(g:cons (list id (g:third s) 131
					(bit:or (if (= (index s 5) nil) 0 (index s 5))
						(if (= (index s 4) nil) 0 64))
					(head s) (index s 3)) acc)
			acc)))))))

;; extern symbols (object mode) go at the end of the section, ids
;; continuing where the defined entries stopped; ATTR_EXTERN marks
;; them undefined for c4rlink, value 0
(define g:extsection (lambda (exts id acc)
	(if (empty? exts) (g:reverse acc)
	(begin
		(define x (head exts))
		(next g:extsection (tail exts) (+ id 1)
			(g:cons (list id (g:second x) 129
				(bit:or 16 (if (g:third x) 32 0))
				(head x) 0) acc))))))

;; rewrite (OP (extph K)) operands into (OP (extern BASE+K)) symbol
;; references once the symbol ids are final
(define g:remapext (lambda (code base acc)
	(if (empty? code) (g:reverse acc)
	(begin
		(define i (head code))
		(next g:remapext (tail code) base (g:cons
			(if (= 2 (length i))
				(if (if (= 'list (typeof (g:second i)))
						(= 'extph (head (g:second i)))
						false)
					(list (head i)
						(list 'extern (+ base (g:second (g:second i)))))
				i)
			i)
			acc))))))

;; ---- module assembly ----

(define gen:module (lambda (ast)
	(begin
		(set! g:code (list))
		(set! g:nlabel 0)
		(set! g:data (string:alloc g:DMAX))
		(set! g:dlen 0)
		(set! g:dpatches (list))
		(set! g:syms (list))
		(set! g:cons* (list))
		(set! g:des* (list))
		(set! g:brk nil)
		(set! g:cont nil)
		(set! g:swcases nil)
		(set! g:swdef nil)
		(set! g:lastarray false)
		(set! g:stubs (list))
		(set! g:externs (list))
		(set! g:nexterns 0)
		(g:sysinit g:syscalls)
		(g:prepass (tail ast))
		(g:protopass (tail ast))
		(g:decls (tail ast))
		(g:emitstubs (g:reverse g:stubs))
		(define m (g:lookup "main" g:syms))
		(if m nil
			(if gen:objmode nil (g:die "no main function")))
		(define syms (g:symsection (g:reverse g:syms) 0 (list)))
		(define code (g:reverse g:code))
		(if (> g:nexterns 0)
			(begin
				(define base (length syms))
				(set! syms (+ syms (g:extsection (g:reverse g:externs) base (list))))
				(set! code (g:remapext code base (list))))
			nil)
		(list 2 64 (if m (list 'code (index m 3)) -1)
			code
			(string:substr g:data 0 g:dlen)
			syms
			(g:reverse g:cons*)
			(g:reverse g:des*)
			(g:reverse g:dpatches)))))

)
