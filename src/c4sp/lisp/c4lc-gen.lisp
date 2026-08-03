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

(define g:dalign (lambda ()
	(if (= 0 (bit:and g:dlen 7)) nil
	(begin (set! g:dlen (+ g:dlen 1)) (next g:dalign)))))

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
;;          (NAME 'glo TYPE BYTEOFF) | (NAME 'loc TYPE IDX)
;; prepend order gives local-over-global shadowing for free

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

(define g:syscalls '(
	("open" OPEN) ("read" READ) ("close" CLOS) ("printf" PRTF)
	("malloc" MALC) ("free" FREE) ("memset" MSET) ("memcmp" MCMP)
	("exit" EXIT) ("putchar" PUTC) ("puts" PUTS) ("realloc" RALC)
	("memcpy" MCPY) ("stacktrace" STRC)))
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

;; step size for ++/--/ptr arithmetic on type ty, as c4cc: > PTR steps
;; a word, everything else (char, int, char*) steps 1
(define g:step (lambda (ty) (if (> ty g:PTR) g:WORD 1)))

;; address of an lvalue in the accumulator; g:ty = the VALUE's type
(define g:addr (lambda (e)
	(begin
		(define h (head e))
		(if (= h 'var) (g:varaddr (g:second e))
		(if (= h 'deref)
			(begin
				(g:expr (g:second e))
				(if (> g:ty g:INT) nil (g:die "bad dereference"))
				(set! g:ty (- g:ty g:PTR)))
		(if (= h 'index)
			(begin
				(g:indexaddr e)
				nil)
		(g:die (+ "bad lvalue: " (+ "" h)))))))))

(define g:varaddr (lambda (n)
	(begin
		(define s (g:find n))
		(define cl (g:second s))
		(if (= cl 'loc) (g:emit (list 'LEA (- g:loc (index s 3))))
		(if (= cl 'glo) (g:emit (list 'IMM (list 'data (index s 3))))
		(g:die (+ "not a variable: " n))))
		(set! g:ty (g:third s)))))

;; base[idx] address; g:ty = element type
(define g:indexaddr (lambda (e)
	(begin
		(g:expr (g:second e))
		(define t g:ty)
		(if (< t g:PTR) (g:die "pointer type expected") nil)
		(g:emit '(PSH))
		(g:expr (g:third e))
		(if (> t g:PTR)
			(begin
				(g:emit '(PSH))
				(g:emit (list 'IMM g:WORD))
				(g:emit '(MUL)))
			nil)
		(g:emit '(ADD))
		(set! g:ty (- t g:PTR)))))

;; ++x / --x (pre): address, load, adjust, store; value = new
(define g:preincdec (lambda (e op)
	(begin
		(g:addr (g:second e))
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
		(g:emit '(PSH))
		(g:expr (g:third e))
		(if (= opname 'ADD)
			(begin
				(if (> t g:PTR)
					(begin
						(g:emit '(PSH))
						(g:emit (list 'IMM g:WORD))
						(g:emit '(MUL)))
					nil)
				(g:emit '(ADD))
				(set! g:ty t))
		(if (= opname 'SUB)
			(if (if (> t g:PTR) (= t g:ty) false)
				(begin        ;; ptr - ptr: difference in elements
					(g:emit '(SUB))
					(g:emit '(PSH))
					(g:emit (list 'IMM g:WORD))
					(g:emit '(DIV))
					(set! g:ty g:INT))
			(begin
				(if (> t g:PTR)
					(begin
						(g:emit '(PSH))
						(g:emit (list 'IMM g:WORD))
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
(define g:call (lambda (e)
	(begin
		(define n (g:second e))
		(define t (g:pushargs (tail (tail e)) 0))
		(define s (g:find n))
		(define cl (g:second s))
		(if (= cl 'sys) (g:emit (list (index s 3)))
		(if (= cl 'fun)
			(begin
				(if (index s 5) (g:die (+ "variadic calls need L3: " n)) nil)
				(if (= t (index s 4)) nil
					(g:die (+ "argument count mismatch in call to " n)))
				(g:emit (list 'JSR (list 'code (index s 3)))))
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
				(define t g:ty)
				(g:emit '(PSH))
				(g:expr (g:third e))
				(g:store! t)
				(set! g:ty t))
		(if (= h 'deref)
			(begin
				(g:expr (g:second e))
				(if (> g:ty g:INT) nil (g:die "bad dereference"))
				(set! g:ty (- g:ty g:PTR))
				(g:load! g:ty))
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
				(g:emit (list 'IMM (if (= (g:second e) g:CHAR) 1 g:WORD)))
				(set! g:ty g:INT))
		(if (= h 'sizeofa) (g:die "sizeof(array) needs L3")
		(if (= h 'index)
			(begin
				(g:indexaddr e)
				(g:load! g:ty))
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
			(g:die (+ "bad expression node: " (+ "" h))))))))))))))))))))))))))))))

(define g:varexpr (lambda (n)
	(begin
		(define s (g:find n))
		(define cl (g:second s))
		(if (= cl 'loc)
			(begin
				(g:emit (list 'LEA (- g:loc (index s 3))))
				(set! g:ty (g:third s))
				(g:load! g:ty))
		(if (= cl 'glo)
			(begin
				(g:emit (list 'IMM (list 'data (index s 3))))
				(set! g:ty (g:third s))
				(g:load! g:ty))
		(if (= cl 'fun)
			(begin
				;; function name as a value: its address
				(g:emit (list 'IMM (list 'code (index s 3))))
				(set! g:ty (g:third s)))
		(g:die (+ "bad variable: " n))))))))

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

(define g:stmt (lambda (s)
	(begin
		(define h (head s))
		(if (= h 'block) (g:stmts (tail s))
		(if (= h 'expr) (g:expr (g:second s))
		(if (= h 'if) (g:ifstmt s)
		(if (= h 'while) (g:whilestmt s)
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
		(if (= h 'switch) (g:die "switch needs L3")
		(g:die (+ "bad statement node: " (+ "" h))))))))))))))))
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

;; ---- declarations ----

;; pre-pass: register every defined function so forward calls resolve
(define g:prepass (lambda (decls)
	(if (empty? decls) nil
	(begin
		(define d (head decls))
		(if (= (head d) 'func)
			(set! g:syms (g:cons
				(list (g:third d) 'fun (g:second d) (g:newlabel)
					(length (index d 3)) (index d 4))
				g:syms))
			nil)
		(next g:prepass (tail decls))))))

;; scalar global: one initialized word (c4cc gives char globals a whole
;; word too); "str" and &fn initializers add data-resident patches
(define g:global (lambda (d)
	(begin
		(define ty (g:second d))
		(define n (g:third d))
		(if (= (index d 3) nil) nil (g:die (+ "arrays need L3: " n)))
		(define init (index d 5))
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
		(set! g:syms (g:cons (list n 'glo ty at) g:syms)))))

;; locals: params idx 0..argc-1, loc = argc+1, scalars idx loc+1...;
;; initializer stores re-run on every entry, emitted after ENT
(define g:params (lambda (ps i)
	(if (empty? ps) i
	(begin
		(set! g:syms (g:cons
			(list (g:second (head ps)) 'loc (head (head ps)) i) g:syms))
		(next g:params (tail ps) (+ i 1))))))

(define g:localdefs (lambda (ls i)
	(if (empty? ls) i
	(begin
		(define d (head ls))   ;; (local TYPE NAME SIZE INIT)
		(if (= (index d 3) nil) nil
			(g:die (+ "local arrays need L3: " (g:third d))))
		(set! g:syms (g:cons
			(list (g:third d) 'loc (g:second d) (+ i 1)) g:syms))
		(next g:localdefs (tail ls) (+ i 1))))))

(define g:localinits (lambda (ls i)
	(if (empty? ls) nil
	(begin
		(define d (head ls))
		(define init (index d 4))
		(if (= init nil) nil
		(begin
			(g:emit (list 'LEA (- g:loc (+ i 1))))
			(g:emit '(PSH))
			(if (= (head init) 'num)
				(g:emit (list 'IMM (g:second init)))
			(if (= (head init) 'str)
				(g:emit (list 'IMM (list 'data (g:dstr (g:second init)))))
			(if (= (head init) 'fnaddr)
				(begin
					(define f (g:find (g:second init)))
					(g:emit (list 'IMM (list 'code (index f 3)))))
			(g:die "unsupported local initializer"))))
			(g:emit '(SI))))
		(next g:localinits (tail ls) (+ i 1))))))

(define g:function (lambda (d)
	(begin
		(define n (g:third d))
		(define ps (index d 3))
		(define attrs (index d 5))
		(define ls (tail (index d 6)))
		(define body (index d 7))
		(define s (g:lookup n g:syms))
		(define oldsyms g:syms)
		(define argc (g:params ps 0))
		(set! g:loc (+ argc 1))
		(define nloc (- (g:localdefs ls (+ argc 1)) argc))
		(g:label! (index s 3))
		(g:emit (list 'ENT (- nloc 1)))
		(g:localinits ls (+ argc 1))
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
		(if (= h 'global) (g:global d)
		(if (= h 'func) (g:function d)
		(g:die (+ "bad declaration node: " (+ "" h))))))))))
(define g:decls (lambda (l)
	(if (empty? l) nil
	(begin
		(g:decl (head l))
		(next g:decls (tail l))))))

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
		(g:sysinit g:syscalls)
		(g:prepass (tail ast))
		(g:decls (tail ast))
		(define m (g:lookup "main" g:syms))
		(if m nil (g:die "no main function"))
		(list 2 64 (list 'code (index m 3))
			(g:reverse g:code)
			(string:substr g:data 0 g:dlen)
			(list)
			(g:reverse g:cons*)
			(g:reverse g:des*)
			(g:reverse g:dpatches)))))

)
