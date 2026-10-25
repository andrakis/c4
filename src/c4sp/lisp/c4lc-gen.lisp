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

;; the function being generated, so an error can say where it was found
;; (AST nodes carry no line numbers; the parser's errors do, these name the function)
(define g:curfn "")
;; ... and its return type and mask, which a return converts to
(define g:curret 1)
(define g:curretq 0)
(define g:die (lambda (msg)
	(error (+ "c4lc gen: " msg (if (= g:curfn "") "" (+ " (in function " g:curfn ")"))))))

;; ---- machine constants (c4cc's) ----

(define g:CHAR 0)
(define g:INT 1)
(define g:PTR 2)
;; target word size follows the host running the compiler, like c4cc's
;; sizeof(int): a 64-bit c4sp emits 64-bit images, a 32-bit build
;; (c4sp32) emits 32-bit ones. c4r.lisp's encoder does the same.
(define g:WORD (sys:wordsize))
(define g:WMASK (- g:WORD 1))

;; M4's scalar band (c4lc-parse.lisp has the encoding). uint is 32 bits
;; at either word size: on a 64-bit host its values are kept in
;; [0, 2^32) -- every load, cast, store and result that could leave that
;; range is masked back into it -- so the signed opcodes give the
;; unsigned answer for it. ulong, and uint on a 32-bit host, are a whole
;; word, compared through the sign-bit flip and divided by a helper.
(define g:UCHAR 512)
(define g:SCHAR 576)
(define g:SHORT 640)
(define g:USHORT 704)
(define g:UINT 768)
(define g:ULONG 832)
;; long long on a 32-bit target (M8, section 14.7): two words. The parser
;; makes these codes only there; on a 64-bit target long long is the word.
(define g:LL 896)
(define g:ULL 960)
;; float and double (the floating-point section below); on a 32-bit
;; target a double is carried exactly as long long is
(define g:FLOAT 1025024)
(define g:DOUBLE 1025088)
(define g:ll? (lambda (t) (if (= t g:LL) true (if (= t g:ULL) true (if (= t g:DOUBLE) (= g:WORD 4) false)))))
(define g:BITS (* 8 g:WORD))
(define g:W64 (= g:WORD 8))
(define g:MIN (bit:shl 1 (- g:BITS 1)))
(define g:M32 (if g:W64 (- (bit:shl 1 32) 1) -1))
;; the const bits of pointer levels 1..14: what an assignment may not
;; quietly lose
(define g:CQ1 357913940)

;; ---- emission state ----

(define g:code nil)       ;; reversed instruction list
(define g:nlabel 0)       ;; next label id
(define g:ty 0)           ;; c4cc's ty: type of the value in the accumulator

(define g:emit (lambda (i) (set! g:code (g:cons i g:code))))
;; the last instruction emitted, past -g's zero-size line and func items
(define g:lastreal (lambda (c)
	(if (empty? c) (list 'none)
	(if (if (= (head (head c)) 'line) true (= (head (head c)) 'func))
		(next g:lastreal (tail c))
	(head c)))))
;; -g: the location of the statement being generated, and a loop's
;; code after its body -- a for's step, the jump back -- on the loop's
;; own line rather than the body's last
(define g:lineloc 0)
(define g:loopline (lambda (loc)
	(if p:lines (g:emit (list 'line loc)) nil)))
(define g:newlabel (lambda ()
	(begin
		(define l g:nlabel)
		(set! g:nlabel (+ l 1))
		l)))
(define g:label! (lambda (l) (g:emit (list 'label l))))

;; The inliner's exit labels (docs/inline-small-functions.md T4b).
;;
;; A body with an early `return` cannot be spliced as a plain block --
;; the return has to go SOMEWHERE, and in the callee that somewhere was
;; LEV. T4 turns each one into a jump to the end of the block it spliced,
;; which is exactly the shape `break` already has, so it reuses the
;; machinery `break` uses rather than inventing any.
;;
;; goto's labels (M7b), by name and per function: the code label made
;; on first mention, whether that is the goto or the label; the names
;; placed so far; and the names some goto wants. g:function resets them
;; and refuses a goto to a label the function never places.
(define g:ulabs (list))
(define g:uplaced (list))
(define g:uwanted (list))
(define g:ulab (lambda (n)
	(begin
		(define e (g:lookup n g:ulabs))
		(if e (g:second e)
		(begin
			(define l (g:newlabel))
			(set! g:ulabs (g:cons (list n l) g:ulabs))
			l)))))
(define g:uplace! (lambda (n)
	(begin
		(if (g:inlist? n g:uplaced) (g:die (+ "duplicate label: " n)) nil)
		(set! g:uplaced (g:cons n g:uplaced))
		(g:label! (g:ulab n)))))
(define g:inlist? (lambda (x l)
	(if (empty? l) false (if (= (head l) x) true (next g:inlist? x (tail l))))))
(define g:ucheck (lambda (l)
	(if (empty? l) nil
	(begin
		(if (g:inlist? (head l) g:uplaced) nil
			(g:die (+ "goto to a label the function does not have: " (head l))))
		(next g:ucheck (tail l))))))

;; The tree pass cannot name a generator label, so it names its own
;; SPLICE, and the mapping is made here on first mention -- which may be
;; the jump rather than the label, since the jump is always forward.
;; Ids are unique across the module and labels reset with it, so one
;; table for the module is right.
(define g:ilabs (list))
(define g:ilab (lambda (id)
	(begin
		(define e (g:lookup id g:ilabs))
		(if e (g:second e)
		(begin
			(define l (g:newlabel))
			(set! g:ilabs (g:cons (list id l) g:ilabs))
			l)))))

;; ---- data segment ----

(define g:DMAX 262144)
(define g:data nil)
(define g:dlen 0)

;; ---- bss (uninitialized data) ----
;;
;; A global with NO initializer occupies no image bytes: instead of
;; writing its zeros into g:data (which both bloats the image and
;; consumes the DMAX build buffer), it is placed in a separate BSS
;; region that lives AFTER all initialized data. Its symbol offset is
;; stored NEGATIVE-ENCODED as -(bssoff+1) -- distinguishable from a
;; real (non-negative) data offset without a type test -- and both the
;; symbol table and the emitted operands are relocated to their final
;; absolute data offset at module finalize (g:module), once the total
;; initialized-data length (the BSS base) is known. g:blen is the BSS
;; byte cursor; the final in-memory data size is (bss base) + g:blen,
;; reported to the loader as the image's MEMSZ so it zero-fills the
;; region at load. Nothing is ever written to g:data for BSS, so a
;; program may declare arbitrarily large uninitialized arrays without
;; approaching DMAX.
(define g:blen 0)

;; align the bss cursor to a word (every allocation starts word-aligned,
;; matching g:dalign's behaviour for the data segment)
(define g:balign (lambda ()
	(if (= 0 (bit:and g:blen g:WMASK)) nil
	(begin (set! g:blen (+ g:blen 1)) (next g:balign)))))

;; reserve nb bss bytes; returns the negative-encoded relative offset
(define g:balloc (lambda (nb)
	(begin
		(g:balign)
		(define at g:blen)
		(set! g:blen (+ g:blen nb))
		(- (- 0 at) 1))))

;; is a 'glo symbol offset a bss (negative-encoded) offset?
(define g:bss? (lambda (off) (< off 0)))
;; decode a negative-encoded bss offset back to its relative byte offset
(define g:bssoff (lambda (off) (- (- 0 off) 1)))

;; the operand for a global at offset `off` (int = data, negative = bss):
;; a (data N) or (bss N) reference, resolved to an absolute (data N) at
;; finalize (g:resolvebss)
(define g:glooperand (lambda (off)
	(if (g:bss? off) (list 'bss (g:bssoff off)) (list 'data off))))

;; padding bytes must be written: the buffer is malloc'd, and garbage
;; padding would make the emitted image nondeterministic
(define g:dalign (lambda ()
	(if (= 0 (bit:and g:dlen g:WMASK)) nil
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

(define g:dpatches nil)   ;; (dcode BYTEOFF LABEL) / (ddata BYTEOFF VALOFF) /
                          ;; (dsym BYTEOFF K): extern K's address (M7b)

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
		(if s s
		(begin
			(define s2 (g:lookup n g:llsyms))
			(if s2 s2
			(begin
				(define hd (g:lookup n g:llsrc))
				(if hd (g:llreg (g:second hd))
				(g:die (+ "undefined identifier: " n))))))))))

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
	("__c4_invoke" C4IV) ("__c4_float" FLT)
	;; F3: __c4_fpu(r, b, a, op), what the float helpers call when the
	;; machine has an FPU (src/c4fp/softfp.c)
	("__c4_fpu" FPU)
	;; c4mp's processor opcodes (66+). c4m does not have these; a
	;; program that calls them must test __c4_info() & C4I_SMP first,
	;; or it gets an illegal instruction there instead of an answer.
	("__c4_cpu_id" CPUI) ("__c4_cpu_count" CPUN)
	("__c4_cpu_start" CPUS) ("__c4_cpu_halt" CPUH)
	("__c4_cas" CAS) ("__c4_xchg" XCHG) ("__c4_fadd" FADD)
	("__c4_wait" CWAI) ("__c4_wake" CWAK) ("__c4_ipi" IPI)
	("__c4_termraw" TRAW)))
;; -fno-builtin (gen:nobuiltin): c4cc's library names are not opcodes but
;; ordinary identifiers, so a C library can define them and its callers
;; call them. Everything from install_trap_handler on stays a builtin:
;; those are the machine's own primitives, and __c4_opcode reaches the
;; opcodes these names stood for.
(define gen:nobuiltin false)
(define g:libcnames '("open" "read" "close" "printf" "malloc" "free" "memset"
	"memcmp" "exit" "putchar" "puts" "realloc" "memcpy" "stacktrace"))
(define g:inlist (lambda (n l)
	(if (empty? l) false
	(if (= (head l) n) true
	(next g:inlist n (tail l))))))
(define g:sysinit (lambda (l)
	(if (empty? l) nil
	(begin
		(if (if gen:nobuiltin (g:inlist (head (head l)) g:libcnames) false)
			nil
			(set! g:syms (g:cons
				(list (head (head l)) 'sys g:INT (g:second (head l))) g:syms)))
		(next g:sysinit (tail l))))))

;; ---- break/continue targets ----

(define g:brk nil)
(define g:cont nil)

;; ---- expressions ----

;; ---- the M4 helpers ----
;;
;; Six small routines the generator calls instead of open-coding what
;; the VM has no opcode for: a 16-bit load (sign- or zero-extending), a
;; 16-bit store, unsigned word division and remainder, and a logical
;; right shift by a run-time count. Each is emitted ONCE, at the end of
;; the unit's code, and only if something called it -- no symbol, no
;; library, no link step: a call is a JSR to a label in the same unit,
;; which is why c4rlink and the loader never hear about them.
(define g:HELPERS '(ld16s ld16u st16 udiv umod lsr))
(define g:helps (list))    ;; ((NAME LABEL) ...) for the helpers used
(define g:helper (lambda (h)
	(begin
		(define e (g:lookup h g:helps))
		(if e (g:second e)
		(begin
			(define l (g:newlabel))
			(set! g:helps (g:cons (list h l) g:helps))
			l)))))
;; the argument(s) are already on the stack but for the one in A
(define g:helpcall (lambda (h n)
	(begin
		(g:emit '(PSH))
		(g:emit (list 'JSR (list 'code (g:helper h))))
		(g:emit (list 'ADJ n)))))
(define g:emitl (lambda (l)
	(if (empty? l) nil
	(begin (g:emit (head l)) (next g:emitl (tail l))))))
(define g:emithelpers (lambda (hs)
	(if (empty? hs) nil
	(begin
		(define e (g:lookup (head hs) g:helps))
		(if e (g:helpbody (head hs) (g:second e)) nil)
		(next g:emithelpers (tail hs))))))
;; One parameter sits at bp+2; of two, the first is at bp+3.
;; -g: a body the compiler made, with no source: an entry in the map's
;; function table, <ld16s> and the like, and a row of line 0
(define g:madebody (lambda (name)
	(if p:lines
		(begin
			(g:emit (list 'func name 0))
			(g:emit (list 'line 0)))
	nil)))
(define g:helpbody (lambda (h l)
	(begin
		(define maxpos (bit:xor g:MIN -1))
		(g:madebody (+ "<" (+ "" h) ">"))
		(g:label! l)
		(if (= h 'ld16s)
			(g:emitl (list '(ENT 0)
				'(LEA 2) '(LI) '(PSH) '(IMM 1) '(ADD) '(LC) '(PSH) '(IMM 8) '(SHL)
				'(PSH) '(LEA 2) '(LI) '(LC) '(PSH) '(IMM 255) '(AND) '(OR) '(LEV)))
		(if (= h 'ld16u)
			(g:emitl (list '(ENT 0)
				'(LEA 2) '(LI) '(PSH) '(IMM 1) '(ADD) '(LC) '(PSH) '(IMM 255) '(AND)
				'(PSH) '(IMM 8) '(SHL)
				'(PSH) '(LEA 2) '(LI) '(LC) '(PSH) '(IMM 255) '(AND) '(OR) '(LEV)))
		(if (= h 'st16)
			(g:emitl (list '(ENT 0)
				'(LEA 3) '(LI) '(PSH) '(LEA 2) '(LI) '(SC)
				'(LEA 3) '(LI) '(PSH) '(IMM 1) '(ADD) '(PSH)
				'(LEA 2) '(LI) '(PSH) '(IMM 8) '(SHR) '(SC)
				'(LEA 2) '(LI) '(LEV)))
		(if (= h 'udiv)
			(begin
				(define l1 (g:newlabel))
				(define l2 (g:newlabel))
				(g:emitl (list '(ENT 1)
					;; a divisor with its top bit set goes in at most once
					'(LEA 2) '(LI) '(PSH) '(IMM 0) '(LT) (list 'BZ (list 'code l1))
					'(LEA 3) '(LI) '(PSH) (list 'IMM g:MIN) '(XOR)
					'(PSH) '(LEA 2) '(LI) '(PSH) (list 'IMM g:MIN) '(XOR) '(GE) '(LEV)
					(list 'label l1)
					;; both halves below the sign bit: the signed divide is right
					'(LEA 3) '(LI) '(PSH) '(IMM 0) '(GE) (list 'BZ (list 'code l2))
					'(LEA 3) '(LI) '(PSH) '(LEA 2) '(LI) '(DIV) '(LEV)
					(list 'label l2)
					;; q = ((a >>> 1) / b) << 1, then at most one more
					'(LEA -1) '(PSH) '(LEA 3) '(LI) '(PSH) '(IMM 1) '(SHR)
					'(PSH) (list 'IMM maxpos) '(AND) '(PSH) '(LEA 2) '(LI) '(DIV)
					'(PSH) '(IMM 1) '(SHL) '(SI)
					'(LEA 3) '(LI) '(PSH) '(LEA -1) '(LI) '(PSH) '(LEA 2) '(LI) '(MUL) '(SUB)
					'(PSH) (list 'IMM g:MIN) '(XOR)
					'(PSH) '(LEA 2) '(LI) '(PSH) (list 'IMM g:MIN) '(XOR) '(GE)
					'(PSH) '(LEA -1) '(LI) '(ADD) '(LEV))))
		(if (= h 'umod)
			(begin
				(define m1 (g:newlabel))
				(define m2 (g:newlabel))
				(define m3 (g:newlabel))
				(define m4 (g:newlabel))
				(g:emitl (list '(ENT 1)
					'(LEA 2) '(LI) '(PSH) '(IMM 0) '(LT) (list 'BZ (list 'code m1))
					'(LEA 3) '(LI) '(PSH) (list 'IMM g:MIN) '(XOR)
					'(PSH) '(LEA 2) '(LI) '(PSH) (list 'IMM g:MIN) '(XOR) '(LT)
					(list 'BZ (list 'code m2))
					'(LEA 3) '(LI) '(LEV)
					(list 'label m2)
					'(LEA 3) '(LI) '(PSH) '(LEA 2) '(LI) '(SUB) '(LEV)
					(list 'label m1)
					'(LEA 3) '(LI) '(PSH) '(IMM 0) '(GE) (list 'BZ (list 'code m3))
					'(LEA 3) '(LI) '(PSH) '(LEA 2) '(LI) '(MOD) '(LEV)
					(list 'label m3)
					;; r = a - (((a >>> 1) / b) << 1) * b, then at most one b off
					'(LEA -1) '(PSH) '(LEA 3) '(LI) '(PSH) '(LEA 3) '(LI) '(PSH) '(IMM 1) '(SHR)
					'(PSH) (list 'IMM maxpos) '(AND) '(PSH) '(LEA 2) '(LI) '(DIV)
					'(PSH) '(IMM 1) '(SHL) '(PSH) '(LEA 2) '(LI) '(MUL) '(SUB) '(SI)
					'(LEA -1) '(LI) '(PSH) (list 'IMM g:MIN) '(XOR)
					'(PSH) '(LEA 2) '(LI) '(PSH) (list 'IMM g:MIN) '(XOR) '(LT)
					(list 'BZ (list 'code m4))
					'(LEA -1) '(LI) '(LEV)
					(list 'label m4)
					'(LEA -1) '(LI) '(PSH) '(LEA 2) '(LI) '(SUB) '(LEV))))
		(if (= h 'lsr)
			(begin
				(define s1 (g:newlabel))
				(g:emitl (list '(ENT 0)
					'(LEA 2) '(LI) (list 'BZ (list 'code s1))
					'(LEA 3) '(LI) '(PSH) '(LEA 2) '(LI) '(SHR)
					'(PSH) '(IMM -1) '(PSH) (list 'IMM g:BITS) '(PSH) '(LEA 2) '(LI) '(SUB)
					'(SHL) '(PSH) '(IMM -1) '(XOR) '(AND) '(LEV)
					(list 'label s1)
					'(LEA 3) '(LI) '(LEV))))
		nil)))))))))

;; ---- types (M4) ----

;; pointer levels of any type: char/int interleave below 512, the band
;; and the structs are 64 apart above it
(define g:plevel (lambda (ty)
	(if (< ty 512) (/ (- ty (bit:and ty 1)) 2)
	(/ (- (- ty 512) (* 64 (/ (- ty 512) 64))) 2))))
;; an integer VALUE type: not a pointer, not a struct
(define g:arith? (lambda (t)
	(if (< t g:STRUCT0) (= 0 (g:plevel t)) false)))
(define g:bytety? (lambda (t)
	(if (= t g:CHAR) true (if (= t g:UCHAR) true (= t g:SCHAR)))))
(define g:halfty? (lambda (t) (if (= t g:SHORT) true (= t g:USHORT))))
;; the usual arithmetic conversions: everything narrower than a word
;; promotes to int, and int < uint < ulong
(define g:promote (lambda (t)
	(if (= t g:UINT) g:UINT (if (= t g:ULONG) g:ULONG (if (g:ll? t) t (if (= t g:FLOAT) t g:INT))))))
(define g:uns? (lambda (t) (if (= t g:UINT) true (if (= t g:ULONG) true (= t g:ULL)))))
(define g:uac (lambda (a b)
	(begin
		(define pa (g:promote a))
		(define pb (g:promote b))
		(if (if (= pa g:ULL) true (= pb g:ULL)) g:ULL
		(if (if (= pa g:LL) true (= pb g:LL)) g:LL
		(if (if (= pa g:ULONG) true (= pb g:ULONG)) g:ULONG
		(if (if (= pa g:UINT) true (= pb g:UINT)) g:UINT
		g:INT)))))))
;; two arithmetic operands of which at least one is unsigned: the only
;; case that generates anything different from what c4 always has
(define g:unsbin? (lambda (a b)
	(if (g:arith? a) (if (g:arith? b)
		(if (g:uns? (g:promote a)) true (g:uns? (g:promote b)))
		false) false)))
(define g:nz (lambda (x) (if (= x nil) 0 x)))
;; c4 gives `a + b` and `a - b` the LEFT operand's type -- char + 1 is
;; a char -- and that stays, for char and int. A band type promotes,
;; because its value is converted on the way back and a short that
;; carried 32768 would never be.
(define g:sumty (lambda (t)
	(if (g:ll? t) t
	(if (if (>= t 512) (g:arith? t) false) g:INT t))))

;; a := a & m
(define g:mask! (lambda (m)
	(begin (g:emit '(PSH)) (g:emit (list 'IMM m)) (g:emit '(AND)))))
;; sign-extend the low n bits of a
(define g:sext! (lambda (n)
	(begin
		(g:emit '(PSH)) (g:emit (list 'IMM (- g:BITS n))) (g:emit '(SHL))
		(g:emit '(PSH)) (g:emit (list 'IMM (- g:BITS n))) (g:emit '(SHR)))))
;; flip the sign bit, which turns unsigned order into signed order
(define g:xmin! (lambda ()
	(begin (g:emit '(PSH)) (g:emit (list 'IMM g:MIN)) (g:emit '(XOR)))))

;; the conversion code for a value of type f now in A, to type t. Only
;; integer targets convert; and a source that already fits emits nothing.
(define g:convert! (lambda (f t)
	(if (if (g:fp? f) true (g:fp? t))
		(if (= f t) nil (g:die "internal: a float or double converted without its helper"))
	(if (g:ll? f)
		(if (g:ll? t) nil
		(begin (g:emit '(LI)) (g:convert! (if (= f g:ULL) g:UINT g:INT) t)))
	(if (g:ll? t) (g:die "internal: a long long is made before its operand, not after")
	(g:convert!/2 f t))))))
(define g:convert!/2 (lambda (f t)
	(if (g:arith? t)
		(if (if (= t g:CHAR) true (= t g:SCHAR))
			(if (if (= f g:CHAR) true (= f g:SCHAR)) nil (g:sext! 8))
		(if (= t g:UCHAR) (if (= f g:UCHAR) nil (g:mask! 255))
		(if (= t g:SHORT)
			(if (if (g:bytety? f) true (= f g:SHORT)) nil (g:sext! 16))
		(if (= t g:USHORT)
			(if (if (= f g:UCHAR) true (= f g:USHORT)) nil (g:mask! 65535))
		(if (= t g:UINT)
			(if g:W64
				(if (if (= f g:UCHAR) true (if (= f g:USHORT) true (= f g:UINT))) nil
					(g:mask! g:M32))
			nil)
		nil)))))
	nil)))
;; ... and the conversion an assignment, a ++ or a return makes. char
;; keeps c4's own rule -- the value of `c = 300` is 300, SC drops the
;; rest -- because changing it would change every program there is; the
;; new types get C's.
(define g:sconv! (lambda (f t)
	(if (if (g:fp? f) true (g:fp? t)) (g:convert! f t)
	(if (g:ll? f)
		(if (g:ll? t) nil
		(begin (g:emit '(LI)) (g:sconv! (if (= f g:ULL) g:UINT g:INT) t)))
	(g:sconv!/2 f t)))))
(define g:sconv!/2 (lambda (f t)
	(if (= t g:CHAR) nil
	(if (= t g:INT) nil
	(if (= t g:ULONG) nil
	(g:convert! f t))))))

;; loads and stores by type
(define g:load! (lambda (ty)
	(if (if (= ty g:CHAR) true (= ty g:SCHAR)) (g:emit '(LC))
	(if (= ty g:UCHAR) (begin (g:emit '(LC)) (g:mask! 255))
	(if (= ty g:SHORT) (g:helpcall 'ld16s 1)
	(if (= ty g:USHORT) (g:helpcall 'ld16u 1)
	(if (if (if (= ty g:UINT) true (= ty g:FLOAT)) g:W64 false) (begin (g:emit '(LI)) (g:mask! g:M32))
	(if (g:ll? ty) nil
	(g:emit '(LI))))))))))
(define g:store! (lambda (ty)
	(if (g:ll? ty) (g:llcopy)
	(if (g:bytety? ty) (g:emit '(SC))
	(if (g:halfty? ty) (g:helpcall 'st16 2)
	(g:emit '(SI)))))))

;; ---- struct types (L7) ----
;; encoding: 1024 + 64*id + 2*ptrlevel (see c4lc-parse.lisp)
(define g:STRUCT0 1024)
(define g:SSTEP 64)
(define g:structs nil)   ;; (ID SIZE MEMBERS), MEMBERS=((NAME OFF TYPE BYTES)...)

(define g:sid (lambda (ty) (/ (- ty g:STRUCT0) g:SSTEP)))
(define g:sptrlevel (lambda (ty)
	(- (- ty g:STRUCT0) (* g:SSTEP (g:sid ty)))))
;; struct VALUE type (pointer level 0)? A row is not one
(define g:svalue? (lambda (ty)
	(if (< ty g:STRUCT0) false (if (g:fpband? ty) false
	(if (= 0 (g:sptrlevel ty)) (not (g:rowty? ty)) false)))))
;; arrays of arrays (M9, c4lc-parse's p:rowtype): a row is a struct-band
;; type with no members, COUNT elements of ELEM. Used as a value it is
;; the address of its first element: it decays to ELEM's pointer.
(define g:rows nil)   ;; ((TYPE ELEM COUNT) ...)
(define g:rowty? (lambda (ty)
	(if (< ty g:STRUCT0) false (if (g:lookup ty g:rows) true false))))
(define g:rowelem (lambda (ty) (g:second (g:lookup ty g:rows))))
(define g:rowcount (lambda (ty) (g:third (g:lookup ty g:rows))))
(define g:decay (lambda (ty) (if (g:rowty? ty) (+ (g:rowelem ty) g:PTR) ty)))
;; pointer of any flavour?
(define g:isptr (lambda (ty) (> (g:plevel ty) 0)))

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
	(if (< ty 512) g:WORD
	(if (g:isptr ty) g:WORD
	(if (if (g:svalue? ty) true (g:rowty? ty)) (g:second (g:sinfo ty))
		(if (g:bytety? ty) 1
		(if (g:halfty? ty) 2
		(if (if (g:ll? ty) true (= ty g:DOUBLE)) 8
		g:WORD)))))))))
;; sizeof an expression (M7b), worked out from its type without
;; evaluating it: a string literal is its bytes and the NUL, an array
;; member its whole storage, and everything else its type's size
(define g:sizeofx (lambda (x)
	(begin
		(define h (head x))
		(if (= h 'str) (+ (length (g:second x)) 1)
		;; an element or what a pointer points at, before a row decays
		(if (if (= h 'index) true (= h 'deref)) (g:tysize (g:lvtype x))
		(if (= h 'member) (g:membsize (g:typeof (g:second x)) (g:third x))
		(if (= h 'arrow) (g:membsize (- (g:typeof (g:second x)) g:PTR) (g:third x))
		(g:tysize (g:typeof x)))))))))
;; a member record is (NAME OFF TYPE BYTES Q); BYTES is set for an array
;; member (and a struct one, whose size it equals)
(define g:membsize (lambda (sty n)
	(begin
		(define m (g:member sty n))
		(if (= (index m 3) nil) (g:tysize (index m 2)) (index m 3)))))
;; size of what a pointer of type ty points at
(define g:elemsize (lambda (ty) (g:tysize (- ty g:PTR))))
;; how far apart an array's elements of type ty are: one- and two-byte
;; types pack, everything else takes whole words
(define g:estride (lambda (ty)
	(begin
		(define s (g:tysize ty))
		;; a row or a struct is its size, which its alignment rounds (M9)
		(if (if (g:rowty? ty) true (g:svalue? ty)) s
		(if (< s g:WORD) s (* g:WORD (/ (+ s g:WMASK) g:WORD)))))))
(define g:awords (lambda (n ty) (/ (+ (* n (g:estride ty)) g:WMASK) g:WORD)))

;; member lookup: (NAME OFF TYPE BYTES Q) in struct-value type ty
(define g:member (lambda (ty n)
	(begin
		(if (g:svalue? ty) nil (g:die (+ "not a struct: ." n)))
		(define m (g:lookup n (g:third (g:sinfo ty))))
		(if m m (g:die (+ "no such member: " n))))))

;; struct/union layout from a (structdef NAME TYPE ISUNION MEMBERS SIZE
;; LAYOUT) node -- the parser bakes its assigned type id into the node,
;; since ids are handed out on FIRST MENTION (possibly a forward pointer
;; reference), not at definition, and lays it out (c4lc-parse's
;; p:slayout, C's rule, M9). g:slayout is the word-per-member rule it
;; replaced, for a node without a layout.
(define g:structdef (lambda (d)
	(begin
		(define ty (g:third d))
		(define isu (index d 3))
		;; the parser's layout (M9), when it made one
		(define ms (if (= (index d 5) nil) (g:slayout (index d 4) isu 0 0 (list))
			(list (index d 5) (g:reverse (index d 6)))))
		(set! g:structs (g:cons
			(list (g:sid ty) (head ms) (g:reverse (g:second ms)))
			g:structs))
		(if (= (index d 7) nil) nil
			(set! g:rows (g:cons (list ty (index d 7) (index d 8)) g:rows))))))
;; returns (SIZE REVMEMBERS)
(define g:slayout (lambda (ms isu off mx acc)
	(if (empty? ms)
		(list (if isu mx off) acc)
	(begin
		(define m (head ms))           ;; (TYPE NAME ASIZE [Q])
		(define mty (head m))
		(define asz (g:third m))
		(define words
			(if (= asz nil)
				(/ (+ (g:tysize mty) g:WMASK) g:WORD)
			(g:awords asz (- mty g:PTR))))
		(define bytes
			(if (= asz nil)
				(if (g:svalue? mty) (g:tysize mty) nil)
			(* asz (g:estride (- mty g:PTR)))))
		(next g:slayout (tail ms) isu
			(if isu off (+ off (* words g:WORD)))
			(if (> (* words g:WORD) mx) (* words g:WORD) mx)
			(g:cons (list (g:second m) off mty bytes (g:nz (index m 3))) acc))))))

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
		(if (if (= h 'var) true (= h 'cmpd)) (if (= h 'var) (g:varaddr (g:second e)) (g:cmpdaddr e))
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
		(if (= cl 'glo) (g:emit (list 'IMM (g:glooperand (index s 3))))
		(if (= cl 'extg) (g:emit (list 'IMM (list 'extph (index s 3))))
		(if (if (= cl 'fun) true (= cl 'ext))
			;; &fn: the function's address. Marked like an array so
			;; assignment and ++/-- reject it as an lvalue.
			(g:emit (list 'IMM (g:funref s)))
		(g:die (+ "not a variable: " n))))))
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
		(g:iexpr (g:third e))
		(if (> (g:elemsize t) 1)
			(begin
				(g:emit '(PSH))
				(g:emit (list 'IMM (g:elemsize t)))
				(g:emit '(MUL)))
			nil)
		(g:emit '(ADD))
		(set! g:ty (- t g:PTR)))))

;; M12, -mcisc: true iff `e` (an 'index node) is a plain `var[expr]`
;; whose base has a statically-known 8-byte scalar element type (int,
;; or any pointer-of-pointer type -- never struct, never char).
;; Deliberately conservative: a base more complex than a bare variable
;; (a struct member, a dereferenced pointer expression, the result of
;; a call, ...) returns false and falls back to the general
;; g:indexaddr path, since establishing its type here would mean
;; either evaluating it twice or duplicating type inference this file
;; already does elsewhere as a side effect of code generation.
(define g:cisc-eligible-index? (lambda (e)
	(begin
		(define base (g:second e))
		(if (= (head base) 'var)
			(begin
				(define s (g:find (g:second base)))
				(define bt (g:third s))
				(if (g:isptr bt)
					(begin
						(define pt (- bt g:PTR))
						;; only c4's own char/int family: a band type
						;; needs a mask or a helper beside the access
						(if (< pt 512)
							(if (= pt g:CHAR) false true)
							false))
					false))
			false))))

;; M12: LXI-based load for a cisc-eligible `arr[idx]`. Same base/idx
;; evaluation order as g:indexaddr, just without the separate scale/
;; add/load steps LXI folds into one opcode.
(define g:indexload-cisc (lambda (e)
	(begin
		(g:expr (g:second e))          ;; a = base
		(define t g:ty)
		(g:emit '(PSH))                 ;; stack=[base]
		(g:expr (g:third e))            ;; a = idx
		(g:emit '(LXI))                  ;; a = *(int*)(base + idx*8)
		(set! g:ty (- t g:PTR))
		(set! g:lastarray false))))

;; M12: SXI-based store for a cisc-eligible `arr[idx] = rhs`. base and
;; idx both stay on the stack (unscaled) until rhs is evaluated, so
;; SXI can do base+idx*8+store in one opcode instead of computing the
;; address first (which would need a third stack slot to hold it
;; across evaluating rhs).
(define g:indexstore-cisc (lambda (lv rhs)
	(begin
		(g:expr (g:second lv))         ;; a = base
		(define pt (- g:ty g:PTR))
		(g:emit '(PSH))                 ;; stack=[base]
		(g:expr (g:third lv))           ;; a = idx
		(g:emit '(PSH))                  ;; stack=[base, idx]
		(g:expr rhs)                      ;; a = value
		(g:emit '(SXI))                    ;; *(int*)(base+idx*8) = a; pop 2
		(set! g:ty pt)
		(set! g:lastarray false))))

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

;; ---- long long on a 32-bit target (M8) ----
;;
;; A long long is two words, low first. Its value is its ADDRESS, as a
;; struct's is: a variable, a member or an element is its own storage,
;; and anything computed goes to a temporary in the frame, below the
;; locals -- g:ltemp hands them out, and they live until the end of the
;; statement that made them (g:stmt). The arithmetic the VM's one word
;; cannot do calls helpers written in C (c4lc-parse's p:llsrc), which
;; the parser adds to a unit that says `long long` and which are compiled
;; only if called.
(define g:tbase 0)      ;; the frame slot the temporaries start after
(define g:tcur 0)       ;; temporary words in use
(define g:tmax 0)       ;; ... at most, in this function
(define g:frames (list))   ;; ((ID WORDS) ...): each function's temporaries
(define g:fid 0)
(define g:llsrc (list))    ;; ((NAME DECL) ...): helpers nobody has called yet
(define g:llsyms (list))   ;; ... and the ones called, kept apart from g:syms's scopes
(define g:framewords (lambda (id l)
	(if (empty? l) 0
	(if (= (head (head l)) id) (g:second (head l))
	(next g:framewords id (tail l))))))
(define g:llname? (lambda (n)
	(if (g:prefix? n "__c4ll_") true
	(if (g:prefix? n "__c4f_") true
	(if (g:prefix? n "__c4d_") true
	(g:prefix? n "__c4fp_"))))))
;; a helper c4lc spliced in itself: one of those names, and static -- a
;; unit may define a function by such a name of its own (src/c4fp/tests
;; compiles softfp.c as it is), and that one is an ordinary function
(define g:helperdef? (lambda (d)
	(if (= (head d) 'func) (if (g:llname? (g:third d)) (> (bit:and (index d 5) 8) 0) false) false)))
(define g:prefix? (lambda (n p)
	(if (> (length n) (length p)) (= (string:substr n 0 (length p)) p) false)))
(define g:llreg (lambda (d)
	(begin
		(set! g:llsyms (g:cons
			(list (g:third d) 'fun (g:second d) (g:newlabel)
				(g:pwords (index d 3) 0) (index d 4) (index d 5)
				(g:nz (index d 8)) (g:ptys (index d 3) (list)) (g:pqs (index d 3) (list)))
			g:llsyms))
		(head g:llsyms))))
;; a two-word temporary: the LEA offset of its low word
;; a function's value that comes back through its caller's pointer, the
;; last argument: a long long's, and a struct's (M12)
(define g:sret? (lambda (ty) (if (g:ll? ty) true (g:svalue? ty))))
;; a temporary of n words, for a struct a call returns
(define g:ltempw (lambda (n)
	(begin
		(set! g:tcur (+ g:tcur n))
		(if (> g:tcur g:tmax) (set! g:tmax g:tcur) nil)
		(- g:loc (+ g:tbase g:tcur)))))
;; the address of struct value e, of type ty, in A (M12): a struct lvalue's
;; own; a call's, its caller's temporary; a compound literal's, its hidden
;; local's
(define g:sval (lambda (e ty what)
	(begin
		(define h (head e))
		(if (= (g:typeof e) ty) nil (g:die (+ "the wrong struct in " what)))
		(if (if (= h 'var) true (if (= h 'deref) true (if (= h 'member) true
				(if (= h 'arrow) true (= h 'index)))))
			(g:addr e)
			(g:expr e))
		(set! g:lastarray false))))
;; The type of &E, E of type t: a pointer to t -- but &A, A an array of a
;; row's shape (a typedef's, M11), is a pointer to that row (M12), as C's
;; &A points at the whole array. OpenTyrian's JE_gammaCorrect(&colors),
;; colors a Palette, SDL_Color[256], indexes (*colorBuffer)[x]; a pointer
;; to a pointer would read the palette's first bytes as an address.
(define g:addrty (lambda (e t)
	(begin
		(define r (g:arrrow e))
		(if r (+ r g:PTR) (+ t g:PTR)))))
;; the row an array variable's storage is, or false
(define g:arrrow (lambda (e)
	(if (= (head e) 'var)
		(begin
			(define s (g:find (g:second e)))
			(define cl (g:second s))
			(define arr (if (if (= cl 'glo) true (if (= cl 'loc) true (= cl 'extg))) (index s 4) nil))
			(if (if (= arr nil) true (not (g:isptr (g:third s)))) false
			(begin
				(define el (- (g:third s) g:PTR))
				(define es (g:tysize el))
				(if (if (> es 0) (= 0 (- arr (* es (/ arr es)))) false)
					(g:rowof el (/ arr es) g:rows)
				false))))
	false)))
(define g:rowof (lambda (el n l)
	(if (empty? l) false
	(if (if (= (g:second (head l)) el) (= (g:third (head l)) n) false) (head (head l))
	(next g:rowof el n (tail l))))))
;; a compound literal (M12): its hidden local initialized here, from its
;; brace list, each time it is reached; its value is the local's, and so is
;; its address
(define g:cmpdinit (lambda (e)
	(begin
		(define s (g:find (g:second e)))
		(g:eminit (list 'local (index e 2) (g:second e) (index e 3) (index e 4)) (index s 3)))))
(define g:cmpd (lambda (e)
	(begin
		(g:cmpdinit e)
		(g:varexpr (g:second e)))))
(define g:cmpdaddr (lambda (e)
	(begin
		(g:cmpdinit e)
		(g:varaddr (g:second e)))))
(define g:ltemp (lambda ()
	(begin
		(set! g:tcur (+ g:tcur 2))
		(if (> g:tcur g:tmax) (set! g:tmax g:tcur) nil)
		(- g:loc (+ g:tbase g:tcur)))))
;; [dst] on the stack, the source's address in A: copy the eight bytes;
;; A is dst afterwards (MCPY's result)
(define g:llcopy (lambda ()
	(begin
		(g:emit '(PSH)) (g:emit '(IMM 8)) (g:emit '(PSH)) (g:emit '(MCPY)) (g:emit '(ADJ 3)))))
(define g:llcall (lambda (n words)
	(begin
		(g:emit (list 'JSR (g:funref (g:find n))))
		(g:emit (list 'ADJ words)))))
;; is an operator on these operands a long long one? A shift is when its
;; left operand is; anything else when either arithmetic operand is
(define g:llop? (lambda (shift lt rt)
	(if (g:arith? lt)
		(if shift (g:ll? (g:promote lt))
		(if (g:arith? rt) (if (g:ll? lt) true (g:ll? rt)) false))
	false)))
(define g:llrty (lambda (shift lt rt) (if shift (g:promote lt) (g:uac lt rt))))
;; a constant into a fresh temporary
(define g:llconst (lambda (lo hi ty)
	(begin
		(define t (g:ltemp))
		(g:emit (list 'LEA t)) (g:emit '(PSH)) (g:emit (list 'IMM lo)) (g:emit '(SI))
		(g:emit (list 'LEA (+ t 1))) (g:emit '(PSH)) (g:emit (list 'IMM hi)) (g:emit '(SI))
		(g:emit (list 'LEA t))
		(set! g:ty ty))))
;; e, as a long long, into the temporary at LEA offset TO: a long long is
;; copied; a word is its low half, its high half its sign (0 for the
;; unsigned types and pointers)
(define g:llinto (lambda (e to)
	(begin
		(define et (g:typeof e))
		(g:emit (list 'LEA to)) (g:emit '(PSH))
		(if (g:ll? et)
			(begin (g:expr e) (g:llcopy))
		(begin
			(g:iexpr e)
			(define st g:ty)
			(g:emit '(SI))
			(g:emit (list 'LEA (+ to 1))) (g:emit '(PSH))
			(if (if (g:arith? st) (g:uns? (g:promote st)) true)
				(g:emit '(IMM 0))
			(begin
				(g:emit (list 'LEA to)) (g:emit '(LI))
				(g:emit '(PSH)) (g:emit (list 'IMM (- g:BITS 1))) (g:emit '(SHR))))
			(g:emit '(SI))))
		(set! g:ty (if (g:ll? et) et (if (g:uns? (g:promote et)) g:ULL g:LL))))))
;; e as a long long: A its address
(define g:llval (lambda (e)
	(if (g:ll? (g:typeof e)) (g:expr e)
	(begin
		(define t (g:ltemp))
		(g:llinto e t)
		(g:emit (list 'LEA t))))))
;; e as one word: a long long's low half
(define g:iexpr (lambda (e)
	(begin
		(g:expr e)
		(if (g:ll? g:ty)
			(begin (g:emit '(LI)) (set! g:ty (if (= g:ty g:ULL) g:UINT g:INT)))
		nil))))
;; e as a truth value: a long long is true when either half is
(define g:texpr (lambda (e)
	(if (if g:fpon (g:fp? (g:typeof e)) false)
		;; a float is true when it is not 0.0 (-0.0 included; a NaN is true)
		(g:expr (list 'ne e (g:fpzero (g:typeof e))))
	(g:texpr/2 e))))
(define g:texpr/2 (lambda (e)
	(begin
		(g:expr e)
		(if (g:ll? g:ty)
			(begin (g:emit '(PSH)) (g:llcall "__c4ll_nz" 1) (set! g:ty g:INT))
		nil))))
;; two words onto the stack for a call, the high one first
(define g:llpush (lambda (e)
	(begin
		(define t (g:ltemp))
		(g:llinto e t)
		(g:emit (list 'LEA (+ t 1))) (g:emit '(LI)) (g:emit '(PSH))
		(g:emit (list 'LEA t)) (g:emit '(LI)) (g:emit '(PSH)))))
(define g:llfns '((add "__c4ll_add") (sub "__c4ll_sub") (mul "__c4ll_mul")
	(band "__c4ll_and") (bor "__c4ll_or") (bxor "__c4ll_xor")))
;; l op r: l into a fresh temporary, which the helper overwrites
(define g:llbinary (lambda (e opname rt)
	(if (g:cmpop? opname) (g:llcompare e opname)
	(if (if (= opname 'SHL) true (= opname 'SHR)) (g:llshift e opname rt)
	(begin
		(define t (g:ltemp))
		(g:llinto (g:second e) t)
		(define fn
			(if (= opname 'DIV) (if (= rt g:ULL) "__c4ll_udiv" "__c4ll_div")
			(if (= opname 'MOD) (if (= rt g:ULL) "__c4ll_umod" "__c4ll_mod")
			(g:second (g:assoc/atom (head e) g:llfns)))))
		(g:emit (list 'LEA t)) (g:emit '(PSH))
		(g:emit (list 'LEA t)) (g:emit '(PSH))
		(g:llval (g:third e))
		(g:emit '(PSH))
		(g:llcall fn 3)
		(g:emit (list 'LEA t))
		(set! g:ty rt))))))
;; a comparison: the helper's -1, 0 or 1 against 0
(define g:llcompare (lambda (e opname)
	(begin
		(define u (= (g:uac (g:typeof (g:second e)) (g:typeof (g:third e))) g:ULL))
		(g:llval (g:second e)) (g:emit '(PSH))
		(g:llval (g:third e)) (g:emit '(PSH))
		(g:llcall (if u "__c4ll_ucmp" "__c4ll_cmp") 2)
		(g:emit '(PSH)) (g:emit '(IMM 0)) (g:emit (list opname))
		(set! g:ty g:INT))))
;; a shift by a word, in place in a fresh temporary
(define g:llshift (lambda (e opname rt)
	(begin
		(define t (g:ltemp))
		(g:llinto (g:second e) t)
		(g:emit (list 'LEA t)) (g:emit '(PSH))
		(g:iexpr (g:third e)) (g:emit '(PSH))
		(g:llcall (if (= opname 'SHL) "__c4ll_shl" (if (= rt g:ULL) "__c4ll_shr" "__c4ll_sar")) 2)
		(g:emit (list 'LEA t))
		(set! g:ty rt))))
;; ~x: both halves, in place
(define g:llnot (lambda (e)
	(begin
		(define t (g:ltemp))
		(g:llinto (g:second e) t)
		(define rt g:ty)
		(g:emit (list 'LEA t)) (g:emit '(PSH)) (g:emit (list 'LEA t)) (g:emit '(LI))
		(g:emit '(PSH)) (g:emit '(IMM -1)) (g:emit '(XOR)) (g:emit '(SI))
		(g:emit (list 'LEA (+ t 1))) (g:emit '(PSH)) (g:emit (list 'LEA (+ t 1))) (g:emit '(LI))
		(g:emit '(PSH)) (g:emit '(IMM -1)) (g:emit '(XOR)) (g:emit '(SI))
		(g:emit (list 'LEA t))
		(set! g:ty rt))))
(define g:llneg (lambda (e)
	(begin
		(define t (g:ltemp))
		(g:llinto (g:second e) t)
		(define rt g:ty)
		(g:emit (list 'LEA t)) (g:emit '(PSH)) (g:emit (list 'LEA t)) (g:emit '(PSH))
		(g:llcall "__c4ll_neg" 2)
		(g:emit (list 'LEA t))
		(set! g:ty rt))))
;; lv = rv
(define g:llassign (lambda (lv rv)
	(begin
		(define t (g:lvtype lv))
		(g:addr lv)
		(if g:lastarray (g:die "bad lvalue in assignment") nil)
		(g:wcheck lv "assignment")
		(g:emit '(PSH))
		(g:llval rv)
		(g:llcopy)
		(set! g:ty t))))
;; ++ / --: the lvalue's address kept in a temporary; a post form copies
;; the old value out first
(define g:llincdec (lambda (lv up post)
	(begin
		(define t (g:lvtype lv))
		(define s (g:ltemp))
		(g:emit (list 'LEA s)) (g:emit '(PSH))
		(g:addr lv)
		(if g:lastarray (g:die "bad lvalue in increment") nil)
		(g:wcheck lv (if up "increment" "decrement"))
		(g:emit '(SI))
		(define old (if post (g:ltemp) 0))
		(if post
			(begin
				(g:emit (list 'LEA old)) (g:emit '(PSH))
				(g:emit (list 'LEA s)) (g:emit '(LI))
				(g:llcopy))
		nil)
		(g:llconst (if up 1 -1) (if up 0 -1) t)
		(g:emit '(PSH))
		(g:emit (list 'LEA s)) (g:emit '(LI)) (g:emit '(PSH))
		(g:emit (list 'LEA s)) (g:emit '(LI)) (g:emit '(PSH))
		;; stack: [&step, p, p]: the helper wants (d, a, b)
		(g:llcall "__c4ll_addr" 3)
		(if post (g:emit (list 'LEA old)) (begin (g:emit (list 'LEA s)) (g:emit '(LI))))
		(set! g:ty t))))

;; ---- floating point (docs/c4-float.md) ----
;;
;; float and double are a band of their own past any struct id the
;; parser hands out (c4lc-parse's p:FLOAT), with the same +2 per pointer
;; level. A float is its binary32 bit pattern, held as a uint is: a
;; word, kept in [0, 2^32) on a 64-bit target. A double is binary64: the
;; word on a 64-bit target; on a 32-bit one two words, carried exactly
;; as long long is there -- g:ll? answers yes for it -- so its storage,
;; arguments, temporaries and returns are long long's. Nothing in the
;; VM knows them: every operation is a call to src/c4fp/softfp.c's
;; helpers (__c4f_* and __c4d_*), which the parser splices into a unit
;; that says float or double, and which g:fpexpr reaches through
;; ordinary calls. Literals arrive as bits (the lexer rounds them).
(define g:fp? (lambda (t) (if (= t g:FLOAT) true (= t g:DOUBLE))))
(define g:fpband? (lambda (t) (if (>= t g:FLOAT) (< t (+ g:DOUBLE 64)) false)))
(define g:fpbin? (lambda (a b) (if (g:fp? a) true (g:fp? b))))
;; the usual arithmetic conversions, with a float in them
(define g:fpuac (lambda (a b) (if (if (= a g:DOUBLE) true (= b g:DOUBLE)) g:DOUBLE g:FLOAT)))
(define g:fpcmp? (lambda (h)
	(if (= h 'eq) true (if (= h 'ne) true (if (= h 'lt) true
	(if (= h 'gt) true (if (= h 'le) true (= h 'ge))))))))
(define g:fparith? (lambda (h)
	(if (= h 'add) true (if (= h 'sub) true (if (= h 'mul) true (= h 'div))))))
(define g:fpbad? (lambda (h)
	(if (= h 'mod) true (if (= h 'shl) true (if (= h 'shr) true
	(if (= h 'band) true (if (= h 'bor) true (= h 'bxor))))))))
(define g:SIGN32 (bit:shl 1 31))
;; 0.0 and 1.0 as literals of type ty
(define g:fplit (lambda (ty lo hi32 w)
	(if (= ty g:FLOAT) (list 'num w g:FLOAT)
	(if g:W64 (list 'num (bit:or (bit:shl hi32 32) lo) g:DOUBLE)
	(list 'llnum lo hi32 g:DOUBLE)))))
(define g:fpzero (lambda (ty) (g:fplit ty 0 0 0)))
(define g:fpone (lambda (ty) (g:fplit ty 0 1072693248 1065353216)))
;; x as type ty: itself, or a cast to it
(define g:fpwrap (lambda (x ty) (if (= (g:typeof x) ty) x (list 'cast ty x))))
(define g:fpname (lambda (ty op) (+ (if (= ty g:FLOAT) "__c4f_" "__c4d_") op)))
;; a helper called with ARGS; its value is of type ty
(define g:fpcall (lambda (name args ty)
	(begin
		(g:call (+ (list 'call name) args))
		(set! g:ty ty))))

;; is this node one g:fpexpr makes?
(define g:fpnode? (lambda (e)
	(begin
		(define h (head e))
		(if (g:fparith? h) (g:fpbin? (g:typeof (g:second e)) (g:typeof (g:third e)))
		(if (g:fpcmp? h) (g:fpbin? (g:typeof (g:second e)) (g:typeof (g:third e)))
		(if (g:fpbad? h) (g:fpbin? (g:typeof (g:second e)) (g:typeof (g:third e)))
		(if (= h 'neg) (g:fp? (g:typeof (g:second e)))
		(if (= h 'bitnot) (g:fp? (g:typeof (g:second e)))
		(if (= h 'lognot) (g:fp? (g:typeof (g:second e)))
		(if (= h 'cast) (if (g:fp? (g:second e)) true (g:fp? (g:typeof (g:third e))))
		(if (= h 'assign)
			(if (g:svalue? (g:lvtype (g:second e))) false
			(if (g:fp? (g:lvtype (g:second e))) true (g:fp? (g:typeof (g:third e)))))
		(if (g:incdec? h) (g:fp? (g:lvtype (g:second e)))
		(if (= h 'cond) (g:fpbin? (g:typeof (g:third e)) (g:typeof (index e 3)))
		false)))))))))))))

(define g:fpexpr (lambda (e)
	(begin
		(define h (head e))
		(if (= h 'cast) (g:fpcast (g:second e) (g:third e))
		(if (= h 'assign) (g:fpassign (g:second e) (g:third e))
		(if (g:incdec? h)
			(g:fpincdec (g:second e) (if (= h 'preinc) true (= h 'postinc)) (if (= h 'postinc) true (= h 'postdec)))
		(if (= h 'neg) (g:fpneg (g:second e))
		(if (= h 'lognot) (g:expr (list 'eq (g:second e) (g:fpzero (g:typeof (g:second e)))))
		(if (= h 'cond) (g:fpcond e)
		(if (g:fpcmp? h) (g:fpcompare e)
		(if (g:fparith? h)
			(begin
				(define ty (g:fpuac (g:typeof (g:second e)) (g:typeof (g:third e))))
				(g:fpcall (g:fpname ty (+ "" h))
					(list (g:fpwrap (g:second e) ty) (g:fpwrap (g:third e) ty)) ty))
		(g:die (+ "invalid operand to " (+ (+ "" h) ": a float or double"))))))))))))))

;; e converted to the float type ty
(define g:fpval (lambda (e ty)
	(begin
		(define et (g:typeof e))
		(if (= et ty) (g:expr e)
		(if (g:fp? et)
			(g:fpcall (if (= ty g:DOUBLE) "__c4f_to_d" "__c4d_to_f") (list e) ty)
		(if (g:isptr et) (g:die "a pointer cannot be converted to a float or double")
		(if (g:svalue? et) (g:die "a struct cannot be converted to a float or double")
		(g:fpcall (g:fromfn et ty) (list e) ty))))))))
;; the helper that makes a float type out of an integer type: by the
;; integer's width and signedness after promotion
(define g:fromfn (lambda (et ty)
	(begin
		(define p (g:promote et))
		(g:fpname ty
			(if (if (= p g:ULL) true (if (= p g:ULONG) g:W64 false)) "from_ull"
			(if (g:uns? p) "from_u"
			(if (if (= p g:LL) true g:W64) "from_ll"
			"from_i")))))))
;; ... and the one that makes integer type t out of float type ft
(define g:tofn (lambda (t ft)
	(begin
		(define p (g:promote t))
		(g:fpname ft
			(if (if (= p g:ULL) true (if (= p g:ULONG) g:W64 false)) "to_ull"
			(if (g:uns? p) "to_u"
			(if (if (= p g:LL) true g:W64) "to_ll"
			"to_i")))))))

;; (cast TY E) with a float on one side
(define g:fpcast (lambda (ty e)
	(if (g:fp? ty) (g:fpval e ty)
	(if (g:isptr ty) (g:die "a float or double cannot be converted to a pointer")
	(if (g:svalue? ty) (g:die "a float or double cannot be converted to a struct")
	(begin
		(g:fpcall (g:tofn ty (g:typeof e)) (list e) g:INT)
		(define rt (g:third (g:find (g:tofn ty (g:typeof e)))))
		(g:convert! rt ty)
		(set! g:ty ty)))))))

;; lv = rv, either side a float
(define g:fpassign (lambda (lv rv)
	(begin
		(define t (g:lvtype lv))
		(if (g:fp? t)
			(begin
				(g:addr lv)
				(if g:lastarray (g:die "bad lvalue in assignment") nil)
				(g:wcheck lv "assignment")
				(g:emit '(PSH))
				(g:fpval rv t)
				(g:store! t)
				(set! g:ty t))
		;; an integer lvalue: the value converted as a cast converts it
		(g:expr (list 'assign lv (list 'cast t rv)))))))

;; -x: the sign bit, or __c4d_neg for a double in two words
(define g:fpneg (lambda (x)
	(begin
		(define t (g:typeof x))
		(if (g:ll? t) (g:fpcall "__c4d_neg" (list x) t)
		(begin
			(g:expr x)
			(g:emit '(PSH))
			(g:emit (list 'IMM (if (= t g:FLOAT) g:SIGN32 g:MIN)))
			(g:emit '(XOR))
			(set! g:ty t))))))

;; the helper's -1 / 0 / 1 / 2 (unordered) to C's answer: a NaN is
;; unequal to everything and in no order with anything
(define g:fpcompare (lambda (e)
	(begin
		(define h (head e))
		(define ty (g:fpuac (g:typeof (g:second e)) (g:typeof (g:third e))))
		(g:fpcall (g:fpname ty "cmp") (list (g:fpwrap (g:second e) ty) (g:fpwrap (g:third e) ty)) g:INT)
		(g:emit '(PSH))
		(if (= h 'eq) (begin (g:emit '(IMM 0)) (g:emit '(EQ)))
		(if (= h 'ne) (begin (g:emit '(IMM 0)) (g:emit '(NE)))
		(if (= h 'lt) (begin (g:emit '(IMM -1)) (g:emit '(EQ)))
		(if (= h 'gt) (begin (g:emit '(IMM 1)) (g:emit '(EQ)))
		(if (= h 'le) (begin (g:emit '(IMM 0)) (g:emit '(LE)))
		(begin                                     ;; ge: 0 or 1
			(g:emit '(IMM -2)) (g:emit '(AND))
			(g:emit '(PSH)) (g:emit '(IMM 0)) (g:emit '(EQ))))))))
		(set! g:ty g:INT))))

(define g:fpcond (lambda (e)
	(begin
		(define tt (g:typeof (g:third e)))
		(define et (g:typeof (index e 3)))
		(define rt (if (g:fp? tt) (if (g:fp? et) (g:fpuac tt et) tt) et))
		(g:texpr (g:second e))
		(define l1 (g:newlabel))
		(define l2 (g:newlabel))
		(g:emit (list 'BZ (list 'code l1)))
		(g:fpval (g:third e) rt)
		(g:emit (list 'JMP (list 'code l2)))
		(g:label! l1)
		(g:fpval (index e 3) rt)
		(g:label! l2)
		(set! g:ty rt))))

;; ++ / -- on a float: x = x +- 1.0, and a post form keeps the old
;; value in a temporary first (the lvalue is evaluated again, as a
;; compound assignment's is)
(define g:fpincdec (lambda (lv up post)
	(begin
		(define t (g:lvtype lv))
		(define step (list (if up 'add 'sub) lv (g:fpone t)))
		(if post
			(begin
				(define old (g:ltemp))
				(g:emit (list 'LEA old)) (g:emit '(PSH))
				(g:expr lv)
				(if (g:ll? t) (g:llcopy) (g:emit '(SI)))
				(g:fpassign lv step)
				(g:emit (list 'LEA old))
				(if (g:ll? t) nil (g:emit '(LI)))
				(set! g:ty t))
		(g:fpassign lv step)))))

;; an argument for a parameter of type pt (nil: none known, a variadic
;; one or an unprototyped call): converted to it, or a float promoted
;; to double -- C's default argument promotion
(define g:fparg (lambda (a pt)
	(begin
		(define at (g:typeof a))
		(if (= pt nil) (if (= at g:FLOAT) (list 'cast g:DOUBLE a) a)
		(if (= at pt) a
		(if (g:fp? pt) (if (g:isptr at) a (list 'cast pt a))
		(if (g:fp? at) (if (g:isptr pt) a (list 'cast pt a))
		a)))))))
;; a return value, converted to the function's type
(define g:fpret (lambda (e)
	(begin
		(define et (g:typeof e))
		(if (= et g:curret) e
		(if (g:fp? g:curret) (list 'cast g:curret e)
		(if (g:fp? et) (list 'cast g:curret e)
		e))))))
;; a constant initializer for an object of float type ty: its literal,
;; an integer made a float, or the sign flipped of one already folded
(define g:fpconst (lambda (it ty)
	(begin
		(define h (head it))
		(if (if (= h 'num) (= (g:nz (index it 2)) ty) false) it
		(if (if (= h 'llnum) (= (index it 3) ty) false) it
		(if (if (= h 'num) (not (g:fp? (g:nz (index it 2)))) false)
			(g:fpint (g:second it) ty)
		;; a float constant initializing a double, or a double a float
		(if (if (= h 'num) (g:fp? (g:nz (index it 2))) (= h 'llnum))
			(p:fvalinit (p:litrat it) ty)
		(g:die "a float or double object needs a constant initializer"))))))))
;; integer v as a literal of float type ty, rounded as the lexer rounds
(define g:fpint (lambda (v ty)
	(begin
		(define neg (< v 0))
		(define ds (+ "" (if neg (- 0 v) v)))
		(if (= ty g:FLOAT)
			(begin
				(define w (lex:fpbits32 ds 0))
				(list 'num (if neg (bit:xor w g:SIGN32) w) g:FLOAT))
		(begin
			(define d (lex:fpbits64 ds 0))
			(define hi (if neg (bit:xor (g:second d) g:SIGN32) (g:second d)))
			(g:fplit g:DOUBLE (head d) hi 0))))))

;; a name's type as a value: a function's is c4's quirk, its return type
;; -- except that a function returning a float is still an address
(define g:vartype (lambda (s)
	(if (if (= (g:second s) 'fun) true (= (g:second s) 'ext))
		(if (g:fp? (g:third s)) g:INT (g:third s))
	(g:third s))))
;; does this unit use floats at all? Set when the parser has spliced the
;; helpers in (g:prepass sees __c4f_add); a unit without them never asks
;; what type an expression is on their account
(define g:fpon false)
;; a return statement, its value converted to the function's type
(define g:fpstmt (lambda (s)
	(if (if (= (head s) 'return) (if (= (g:second s) nil) false true) false)
		(list 'return (g:fpret (g:second s)))
	s)))
(define g:fplit? (lambda (it)
	(if (= (head it) 'llnum) (= (index it 3) g:DOUBLE)
	(if (= (head it) 'num) (g:fp? (g:nz (index it 2))) false))))
;; a global's initializer: made of the object's float type, and a float
;; constant initializing an integer converted to it, as C converts. A
;; folded one (fval, c4lc-parse's p:foldinit) is made the object's type.
(define g:fpinit (lambda (it ty)
	(if (= it nil) nil
	(if (= (head it) 'fval) (p:fvalinit (g:second it) ty)
	(if (g:fp? ty) (g:fpconst it ty)
	(if (g:fplit? it) (p:fvalinit (p:litrat it) ty)
	it))))))
;; a local aggregate's float element, stored when the declaration runs
(define g:fplstore (lambda (ty off it)
	(begin
		(g:laddr off)
		(g:emit '(PSH))
		(g:fpval (if (= (head it) 'expr) (g:second it) it) ty)
		(g:store! ty))))

;; ---- what an expression IS, without generating it (M4) ----
;;
;; The generator learns an operand's type by generating it, which is
;; one operand too late for a binary operator: by the time the right
;; side says "unsigned", the left side has been pushed and nothing in
;; the VM can reach it again. So the right side's type is asked for
;; first, by this -- which answers exactly what g:expr would leave in
;; g:ty, quirks included (char + 1 is char, ?: has its else-arm's type).
(define g:last (lambda (l) (if (empty? (tail l)) (head l) (next g:last (tail l)))))
(define g:incdec? (lambda (h)
	(if (= h 'preinc) true (if (= h 'predec) true (if (= h 'postinc) true (= h 'postdec))))))
(define g:cmpop? (lambda (o)
	(if (= o 'EQ) true (if (= o 'NE) true (if (= o 'LT) true
	(if (= o 'GT) true (if (= o 'LE) true (= o 'GE))))))))
(define g:lvtype (lambda (e)
	(begin
		(define h (head e))
		(if (if (= h 'var) true (= h 'cmpd)) (if (= h 'var) (g:third (g:find (g:second e))) (index e 2))
		(if (= h 'deref) (- (g:typeof (g:second e)) g:PTR)
		(if (= h 'index) (- (g:typeof (g:second e)) g:PTR)
		(if (= h 'member) (g:third (g:member (g:typeof (g:second e)) (g:third e)))
		(if (= h 'arrow) (g:third (g:member (- (g:typeof (g:second e)) g:PTR) (g:third e)))
		g:INT))))))))
(define g:unres (lambda (t) (if (g:fp? t) t (if (g:uns? (g:promote t)) (g:promote t) (if (g:ll? t) t g:INT)))))
(define g:typeof (lambda (e)
	(begin
		(define h (head e))
		(if (= h 'num) (if (= (index e 2) nil) g:INT (index e 2))
		(if (= h 'llnum) (index e 3)
		(if (= h 'str) g:PTR
		(if (= h 'var) (g:vartype (g:find (g:second e)))
		(if (if (= h 'call) true (= h 'cmpd)) (if (= h 'call) (g:third (g:find (g:second e))) (index e 2))
		(if (= h 'calle) g:INT
		(if (= h 'assign) (g:lvtype (g:second e))
		(if (= h 'deref) (g:decay (- (g:typeof (g:second e)) g:PTR))
		(if (= h 'addr) (g:addrty (g:second e) (g:lvtype (g:second e)))
		(if (= h 'lognot) g:INT
		(if (if (= h 'bitnot) true (= h 'neg)) (g:unres (g:typeof (g:second e)))
		(if (g:incdec? h) (g:lvtype (g:second e))
		(if (= h 'cast) (g:second e)
		(if (if (= h 'sizeof) true (if (= h 'sizeofa) true (= h 'sizeofe))) g:INT
		(if (if (= h 'index) true (if (= h 'member) true (= h 'arrow))) (g:decay (g:lvtype e))
		(if (= h 'cond) (g:condtype e)
		(if (if (= h 'land) true (= h 'lor)) g:INT
		(if (= h 'comma) (g:typeof (g:last (tail e)))
		(g:bintype e))))))))))))))))))))))
;;
;; Each operand is typed once, and its type handed down: typing one
;; twice per level, as this did, takes 2^N steps on a chain of N
;; (a + a + ... + a), and 3^N on a + (a + (...)). g:ltypes, below,
;; gives the left operand's type when the caller already has it.
(define g:bintype (lambda (e)
	(g:bintype/lt e (g:typeof (g:second e)))))
(define g:bintype/lt (lambda (e lt0)
	(begin
		(define h (head e))
		(define shf (if (= h 'shl) true (= h 'shr)))
		(define rt0 (if shf g:INT (g:typeof (g:third e))))
		(if (g:fpbin? lt0 rt0) (if (g:fpcmp? h) g:INT (g:fpuac lt0 rt0))
		(if (g:llop? shf lt0 rt0)
			(if (if (= h 'eq) true (if (= h 'ne) true (if (= h 'lt) true
					(if (= h 'gt) true (if (= h 'le) true (= h 'ge))))))
				g:INT
				(g:llrty shf lt0 rt0))
			(g:bintype/2 e lt0 rt0))))))
;; rt is the right operand's type, except after a shift, which does not
;; look at it
(define g:bintype/2 (lambda (e lt rt)
	(begin
		(define h (head e))
		;; int + pointer is the pointer's (M13): Rise of the Triad's
		;; *(dest+bufferofs), dest an int
		(if (if (= h 'add) (if (g:isptr lt) false (g:isptr rt)) false)
			rt
		(if (if (= h 'add) true (= h 'sub))
			(if (g:isptr lt)
				(if (= h 'add) lt
				(if (if (> (g:elemsize lt) 1) (= lt rt) false) g:INT lt))
			(if (g:unsbin? lt rt) (g:uac lt rt) (g:sumty lt)))
		(if (if (= h 'shl) true (= h 'shr))
			(if (g:arith? lt) (g:unres lt) g:INT)
		(if (if (g:unsbin? lt rt)
				(not (if (= h 'eq) true (if (= h 'ne) true (if (= h 'lt) true
					(if (= h 'gt) true (if (= h 'le) true (= h 'ge)))))))
				false)
			(g:uac lt rt)
		g:INT)))))))
(define g:condtype (lambda (e)
	(begin
		(define tt (g:typeof (g:third e)))
		(define et (g:typeof (index e 3)))
		(if (g:fpbin? tt et) (if (g:fp? tt) (if (g:fp? et) (g:fpuac tt et) tt) et)
		(if (g:llop? false tt et) (g:uac tt et)
		(if (g:unsbin? tt et) (g:uac tt et) et))))))

;; ---- and what it is qualified with ----
;;
;; The mask of an expression: two bits per pointer level, level 0 the
;; object the expression designates. Only const is enforced; volatile
;; needs nothing here, because c4 emits every access it is asked for
;; and caches none (the tree pass is where it matters -- see
;; c4lc-tree.lisp).
(define g:symq (lambda (s)
	(begin
		(define cl (g:second s))
		(if (= cl 'loc) (g:nz (index s 5))
		(if (= cl 'glo) (g:nz (index s 6))
		(if (= cl 'extg) (g:nz (index s 5))
		0))))))
(define g:fnrq (lambda (s)
	(if (if (= (g:second s) 'fun) true (= (g:second s) 'ext)) (g:nz (index s 7)) 0)))
;; a member inherits the qualifiers of the struct it is read out of; an
;; ARRAY member hands them to its elements
(define g:memq (lambda (st n sq)
	(begin
		(define m (g:member st n))
		(bit:or (g:nz (index m 4))
			(if (if (= (index m 3) nil) true (g:svalue? (g:third m))) sq (bit:shl sq 2))))))
(define g:qualof (lambda (e)
	(begin
		(define h (head e))
		(if (= h 'var) (g:symq (g:find (g:second e)))
		(if (= h 'call) (g:fnrq (g:find (g:second e)))
		(if (if (= h 'deref) true (= h 'index)) (bit:shr (g:qualof (g:second e)) 2)
		(if (= h 'addr) (bit:shl (g:qualof (g:second e)) 2)
		(if (= h 'member)
			(g:memq (g:typeof (g:second e)) (g:third e) (bit:and (g:qualof (g:second e)) 3))
		(if (= h 'arrow)
			(g:memq (- (g:typeof (g:second e)) g:PTR) (g:third e)
				(bit:and (bit:shr (g:qualof (g:second e)) 2) 3))
		(if (= h 'cast) (g:nz (index e 3))
		(if (if (= h 'assign) true (g:incdec? h)) (g:qualof (g:second e))
		(if (= h 'cond) (bit:or (g:qualof (g:third e)) (g:qualof (index e 3)))
		(if (= h 'comma) (g:qualof (g:last (tail e)))
		0)))))))))))))

;; the diagnostics, worded identically in c4fc (src/c4fc/gen.f)
(define g:wcheck (lambda (lv what)
	(if (= 1 (bit:and (g:qualof lv) 1))
		(begin
			(print (+ "error: " (+ what (+ " of read-only location (in function "
				(+ g:curfn ")")))))
			(error "c4lc gen: const violation"))
	nil)))
(define g:dropped? (lambda (lq rq)
	(> (bit:and (bit:and rq (bit:xor lq -1)) g:CQ1) 0)))
(define g:qwarn (lambda (what)
	(print (+ "warning: " (+ what (+ " discards const qualifier (in function "
		(+ g:curfn ")")))))))

;; ++x / --x (pre): address, load, adjust, store; value = new. The
;; post forms store the new value unconverted and take the step back
;; off it -- converting first would lose the carry the undo needs, and
;; the loads convert anyway.
(define g:preincdec (lambda (e op)
	(g:incdec e op false)))
(define g:incdec (lambda (e op post)
	(begin
		(g:addr (g:second e))
		(if g:lastarray (g:die "bad lvalue in pre-increment") nil)
		(define t g:ty)
		(g:wcheck (g:second e) (if (= op 'ADD) "increment" "decrement"))
		(g:emit '(PSH))
		(g:load! t)
		(g:emit '(PSH))
		(g:emit (list 'IMM (g:step t)))
		(g:emit (list op))
		(if post nil (g:sconv! (g:promote t) t))
		(g:store! t)
		(set! g:ty t))))

;; x++ / x-- (post): as pre, then undo the adjustment in the result
(define g:postincdec (lambda (e op unop)
	(begin
		(g:incdec e op true)
		(define t g:ty)
		(g:emit '(PSH))
		(g:emit (list 'IMM (g:step t)))
		(g:emit (list unop))
		(set! g:ty t))))

;; The types down an expression's left spine, while it is binary
;; operators: (type-of x, type-of its left operand, ...), in one pass.
(define g:ltypes (lambda (x)
	(if (g:assoc/atom (head x) g:binops)
		(begin
			(define ls (g:ltypes (g:second x)))
			(g:cons (g:bintype/lt x (head ls)) ls))
	(list (g:typeof x)))))
;; (NODE . TYPES): g:ltypes of NODE's left operand, left by g:binary/1
;; for the g:binary of NODE, its left operand, while that is generated.
;; A chain a + a + ... + a is then typed once, not once per level (N^2).
(define g:lspine false)

;; binary operator with c4cc's pointer scaling -- and, when an operand
;; is unsigned, the usual arithmetic conversions (g:ubinary)
(define g:binary (lambda (e opname)
	(begin
		(define sp (if (if g:lspine (= e (head g:lspine)) false)
			(tail g:lspine)
			(g:ltypes (g:second e))))
		(set! g:lspine false)
		(g:binary/0 e opname sp))))
(define g:binary/0 (lambda (e opname sp)
	;; int + pointer as pointer + int (M13): the pointer's arithmetic
	(if (if (= opname 'ADD) (if (g:isptr (head sp)) false (g:isptr (g:typeof (g:third e)))) false)
		(g:binary (list (head e) (g:third e) (g:second e)) opname)
	(begin
		(define shf (if (= opname 'SHL) true (= opname 'SHR)))
		(define lt0 (head sp))
		(define rtl (if shf g:INT (g:typeof (g:third e))))
		(if (g:llop? shf lt0 rtl) (g:llbinary e opname (g:llrty shf lt0 rtl))
		(g:binary/1 e opname sp))))))
(define g:binary/1 (lambda (e opname sp)
	(begin
		(define rt0 (g:typeof (g:third e)))
		(define l (g:second e))
		(set! g:lspine (if (g:assoc/atom (head l) g:binops) (g:cons l (tail sp)) false))
		(g:expr l)
		(set! g:lspine false)
		(define t g:ty)
		(if (if (if (= opname 'SHL) true (= opname 'SHR))
				(if (g:arith? t) (g:uns? (g:promote t)) false)
				(g:unsbin? t rt0))
			(g:ubinary e opname t rt0)
			(g:binary/2 e opname t)))))

;; uint on a 64-bit host ("narrow"): operands in [0, 2^32), so the
;; signed opcodes are right once the non-uint side of a compare or a
;; division is cut into range, and a result that can leave it is cut
;; back. A whole unsigned word: flip the sign bit for an ordering,
;; call a helper to divide, mask a right shift.
(define g:ubinary (lambda (e opname t rt)
	(begin
		(define shift (if (= opname 'SHL) true (= opname 'SHR)))
		(define r (if shift (g:promote t) (g:uac t rt)))
		(define narrow (if (= r g:UINT) g:W64 false))
		(define cmp (g:cmpop? opname))
		(define ord (if cmp (if (= opname 'EQ) false (if (= opname 'NE) false true)) false))
		(define dm (if (= opname 'DIV) true (= opname 'MOD)))
		(if narrow
			(if (if (if cmp true dm) (not (= (g:promote t) g:UINT)) false) (g:mask! g:M32) nil)
			(if ord (g:xmin!) nil))
		(g:emit '(PSH))
		(g:iexpr (g:third e))
		(define rt2 g:ty)
		(if narrow
			(begin
				(if (if (if cmp true dm) (not (= (g:promote rt2) g:UINT)) false) (g:mask! g:M32) nil)
				(g:emit (list opname))
				(if (if (= opname 'ADD) true (if (= opname 'SUB) true (if (= opname 'MUL) true
						(if (= opname 'SHL) true (if (= opname 'OR) true (= opname 'XOR))))))
					(g:mask! g:M32) nil))
		(begin
			(if ord (g:xmin!) nil)
			(if dm (g:helpcall (if (= opname 'DIV) 'udiv 'umod) 2)
			(if (= opname 'SHR)
				(begin
					(define k (if (= (head (g:third e)) 'num) (g:second (g:third e)) nil))
					(if (= k nil) (g:helpcall 'lsr 2)
					(begin
						(g:emit '(SHR))
						(if (if (> k 0) (< k g:BITS) false)
							(g:mask! (bit:xor (bit:shl -1 (- g:BITS k)) -1))
						nil))))
			(g:emit (list opname))))))
		(set! g:ty (if cmp g:INT r)))))

(define g:binary/2 (lambda (e opname t)
	(begin
		(define es (if (g:isptr t) (g:elemsize t) 1))
		(g:emit '(PSH))
		(g:iexpr (g:third e))
		(if (= opname 'ADD)
			(begin
				(if (> es 1)
					(begin
						(g:emit '(PSH))
						(g:emit (list 'IMM es))
						(g:emit '(MUL)))
					nil)
				(g:emit '(ADD))
				(set! g:ty (g:sumty t)))
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
				(set! g:ty (g:sumty t))))
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
		(define s (g:find n))
		(define cl (g:second s))
		;; a known function's parameters (types, masks) for the const check
		(define kf (if (= cl 'fun) true (= cl 'ext)))
		(define t0 (g:pushargs (tail (tail e)) 0
			(if kf (index s 8) nil) (if kf (index s 9) nil)
			;; a call through a variable holding a pointer: its arguments as written
			(if (if (= cl (quote loc)) true (if (= cl (quote glo)) true (= cl (quote extg)))) "a call through a pointer" n)))		(define llr (if kf (g:sret? (g:third s)) false))
		(define rtmp (if llr (if (g:ll? (g:third s)) (g:ltemp) (g:ltempw (g:pw (g:third s)))) 0))
		(define t t0)
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
				(if llr
					(begin (g:emit (list 'LEA rtmp)) (g:emit '(PSH)) (set! t (+ t 1)))
				nil)
				(g:emit (list 'JSR (g:funref s))))
		(if (= cl 'glo) (g:emit (list 'JSRI (g:glooperand (index s 3))))
		(if (= cl 'extg) (g:emit (list 'JSRI (list 'extph (index s 3))))
		(if (= cl 'loc) (g:emit (list 'JSRS (- g:loc (index s 3))))
		(g:die (+ "bad function call: " n)))))))
		(if (> t 0) (g:emit (list 'ADJ t)) nil)
		(if llr (g:emit (list 'LEA rtmp)) nil)
		(set! g:ty (g:third s)))))
;; A struct passed by value (M9): its words, the last pushed first, so
;; the first lands at the lowest address -- the parameter's -- as a long
;; long's do. Its address waits in a temporary while they go, and the
;; callee has a copy of its own to change.
(define g:structpush (lambda (a ty fname)
	(begin		(if (= (g:typeof a) ty) nil (g:die (+ "passing the wrong struct to " fname)))
		;; the temporary is free again once the words are pushed
		(define ts g:tcur)
		(define tmp (g:ltemp))		(g:emit (list 'LEA tmp)) (g:emit '(PSH))
		(g:sval a ty (+ "a call to " fname))
		(g:emit '(SI))
		(g:structpush/2 tmp (- (g:pw ty) 1))
		(set! g:tcur ts))))
(define g:structpush/2 (lambda (tmp j)
	(if (< j 0) nil
	(begin
		(g:emit (list 'LEA tmp)) (g:emit '(LI))
		(if (> j 0)
			(begin (g:emit '(PSH)) (g:emit (list 'IMM (* j g:WORD))) (g:emit '(ADD)))
			nil)
		(g:emit '(LI)) (g:emit '(PSH))
		(next g:structpush/2 tmp (- j 1))))))
;; struct = struct (M7b): the bytes, by the VM's own memcpy -- what a
;; builtin memcpy(&L, &R, sizeof L) compiles to, so it needs no libc and
;; works under -fno-builtin. The right side must be an lvalue: a struct
;; returned by value is not c4's.
(define g:structassign (lambda (lv rv)
	(begin		(define sty (g:lvtype lv))
		(g:addr lv)
		(g:emit '(PSH))
		(g:sval rv sty "an assignment")
		(g:emit '(PSH))
		(g:emit (list 'IMM (g:tysize sty)))
		(g:emit '(PSH))
		(g:emit '(MCPY))
		(g:emit '(ADJ 3))
		(set! g:ty sty))))
;; (*fp)(x) calls fp, a function pointer being a word here; but (*pf)(x),
;; pf a pointer to one (M13), calls what pf points at
(define g:calltgt (lambda (e)
	(if (= (head e) 'deref)
		(if (g:isptr (g:typeof (g:second e))) e (next g:calltgt (g:second e)))
	e)))
;; a call through an expression (M7b): the arguments, then what JSR
;; would have pushed -- the return address, here an IMM of the label
;; after the jump -- then the target in A and JMPA to it. The callee
;; finds the frame a JSR builds. The value is a word.
(define g:callexpr (lambda (e)
	(begin
		(define t (g:pushargs (tail (tail e)) 0 nil nil "a call through a pointer"))
		(define l (g:newlabel))
		(g:emit (list 'IMM (list 'code l)))
		(g:emit '(PSH))
		(g:expr (g:calltgt (g:second e)))
		(g:emit '(JMPA))
		(g:label! l)
		(if (> t 0) (g:emit (list 'ADJ t)) nil)
		(set! g:ty g:INT))))
(define g:pushargs (lambda (args t pts pqs fname)
	(if (empty? args) t
	(begin
		(define pt (if (if (= pts nil) true (empty? pts)) nil (head pts)))
		;; a float argument: converted to its parameter's type, or promoted
		;; to double -- but a float helper's own arguments go as they are
		(define a (if g:fpon
			(if (g:llname? fname) (head args)
			;; through a pointer the parameters are unknown: as written
			(if (if (= pt nil) (= fname "a call through a pointer") false) (head args)
			(g:fparg (head args) pt)))
			(head args)))
		(define wide (if (= pt nil) (g:ll? (g:typeof a)) (g:ll? pt)))
		;; a struct by value: the parameter's type, or as written
		(define sty (if (= pt nil) (g:typeof a) pt))
		(define byval (if wide false (g:svalue? sty)))
		(if wide (g:llpush a)
		(if byval (g:structpush (head args) sty fname)
		(g:pusharg (g:cons a (tail args)) t pts pqs fname)))
		(next g:pushargs (tail args) (+ t (if wide 2 (if byval (g:pw sty) 1)))
			(if (= pts nil) nil (if (empty? pts) pts (tail pts)))
			(if (= pqs nil) nil (if (empty? pqs) pqs (tail pqs)))
			fname)))))
(define g:pusharg (lambda (args t pts pqs fname)
	(begin
		(g:iexpr (head args))
		(if (if (= pts nil) false (empty? pts)) nil
		(if (= pts nil) nil
		(if (g:isptr (head pts))
			(if (g:dropped? (if (if (= pqs nil) true (empty? pqs)) 0 (head pqs))
					(g:qualof (head args)))
				(g:qwarn (+ "passing argument " (+ (+ "" (+ t 1)) (+ " of " fname))))
			nil)
		nil)))
		(g:emit '(PSH)))))

(define g:comma (lambda (es)
	(if (empty? (tail es)) (g:expr (head es))
	(begin
		(g:expr (head es))
		(next g:comma (tail es))))))

(define g:expr (lambda (e)
	(if g:fpon (if (g:fpnode? e) (g:fpexpr e) (g:expr/0 e)) (g:expr/0 e))))
(define g:expr/0 (lambda (e)
	(begin
		(define h (head e))
		(if (if (= h 'num) (g:ll? (g:nz (index e 2))) false)
			(g:llconst (g:second e) (if (< (g:second e) 0) -1 0) (index e 2))
		(if (= h 'llnum) (g:llconst (g:second e) (g:third e) (index e 3))
		(if (= h 'num)
			(begin
				(g:emit (list 'IMM (g:second e)))
				(set! g:ty (if (= (index e 2) nil) g:INT (index e 2))))
		(if (= h 'str)
			(begin
				(g:emit (list 'IMM (list 'data (g:dstr (g:second e)))))
				(set! g:ty g:PTR))
		(if (= h 'var) (g:varexpr (g:second e))
		(if (if (= h 'call) true (= h 'cmpd)) (if (= h 'call) (g:call e) (g:cmpd e))
		(if (if (= h 'assign) (g:svalue? (g:lvtype (g:second e))) false)
			(g:structassign (g:second e) (g:third e))
		(if (if (= h 'assign) (g:ll? (g:lvtype (g:second e))) false)
			(g:llassign (g:second e) (g:third e))
		(if (= h 'assign)
			(begin
				(define lv (g:second e))
				(if (if gen:cisc
						(if (= (head lv) 'index) (g:cisc-eligible-index? lv) false)
						false)
					(g:indexstore-cisc lv (g:third e))
					(begin
						(g:addr lv)
						(if g:lastarray (g:die "bad lvalue in assignment") nil)
						(define t g:ty)
						(g:wcheck lv "assignment")
						(g:emit '(PSH))
						(g:expr (g:third e))
						(g:sconv! g:ty t)
						(if (g:isptr t)
							(if (g:dropped? (g:qualof lv) (g:qualof (g:third e)))
								(g:qwarn "assignment") nil)
						nil)
						(g:store! t)
						(set! g:ty t))))
		(if (= h 'deref)
			(begin
				(g:expr (g:second e))
				(if (g:isptr g:ty) nil (g:die "bad dereference"))
				(set! g:ty (- g:ty g:PTR))
				(if (g:svalue? g:ty)
					(set! g:lastarray true)   ;; *structptr is an address
				(if (g:rowty? g:ty)
					(begin (set! g:ty (g:decay g:ty)) (set! g:lastarray true))
				(g:load! g:ty))))		(if (= h 'addr)
			(begin
				(g:addr (g:second e))
				(set! g:ty (g:addrty (g:second e) g:ty)))
		(if (= h 'lognot)
			(begin
				(g:texpr (g:second e))
				(g:emit '(PSH))
				(g:emit '(IMM 0))
				(g:emit '(EQ))
				(set! g:ty g:INT))
		(if (if (= h 'bitnot) (g:ll? (g:typeof (g:second e))) false) (g:llnot e)
		(if (if (= h 'neg) (g:ll? (g:typeof (g:second e))) false) (g:llneg e)
		(if (if (g:incdec? h) (g:ll? (g:lvtype (g:second e))) false)
			(g:llincdec (g:second e) (if (if (= h 'preinc) true (= h 'postinc)) true false)
				(if (= h 'postinc) true (= h 'postdec)))
		(if (= h 'bitnot)
			(begin
				(g:expr (g:second e))
				(g:emit '(PSH))
				(g:emit '(IMM -1))
				(g:emit '(XOR))
				(set! g:ty (g:unres g:ty))
				(if (if (= g:ty g:UINT) g:W64 false) (g:mask! g:M32) nil))
		(if (= h 'neg)
			(begin
				(g:emit '(IMM -1))
				(g:emit '(PSH))
				(g:expr (g:second e))
				(g:emit '(MUL))
				(set! g:ty (g:unres g:ty))
				(if (if (= g:ty g:UINT) g:W64 false) (g:mask! g:M32) nil))
		(if (= h 'preinc) (g:preincdec e 'ADD)
		(if (= h 'predec) (g:preincdec e 'SUB)
		(if (= h 'postinc) (g:postincdec e 'ADD 'SUB)
		(if (= h 'postdec) (g:postincdec e 'SUB 'ADD)
		(if (if (= h 'cast) (g:ll? (g:second e)) false)
			(begin
				(g:llval (g:third e))
				(set! g:ty (g:second e)))
		(if (= h 'cast)
			(begin
				(g:expr (g:third e))
				(g:convert! g:ty (g:second e))
				(set! g:ty (g:second e)))
		(if (= h 'sizeof)
			(begin
				(g:emit (list 'IMM (g:tysize (g:second e))))
				(set! g:ty g:INT))
		(if (= h 'sizeofa)
			(begin
				;; an array's storage; anything else, its type's size
				(define sa (g:find (g:second e)))
				(g:emit (list 'IMM (if (= (index sa 4) nil) (g:tysize (g:third sa)) (index sa 4))))
				(set! g:ty g:INT))
		(if (= h 'sizeofe)
			(begin
				(g:emit (list 'IMM (g:sizeofx (g:second e))))
				(set! g:ty g:INT))
		(if (= h 'calle) (g:callexpr e)
		(if (= h 'index)
			(if (if gen:cisc (g:cisc-eligible-index? e) false)
				(g:indexload-cisc e)
				(begin
					(g:indexaddr e)
					(if (g:svalue? g:ty)
						(set! g:lastarray true)   ;; struct element: address
					(if (g:rowty? g:ty)
						(begin                    ;; a row: its first element's address
							(set! g:ty (g:decay g:ty))
							(set! g:lastarray true))
					(g:load! g:ty)))))
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
				(g:texpr (g:second e))
				(define l (g:newlabel))
				(g:emit (list 'BZ (list 'code l)))
				(g:texpr (g:third e))
				(g:label! l)
				(set! g:ty g:INT))
		(if (= h 'lor)
			(begin
				(g:texpr (g:second e))
				(define l2 (g:newlabel))
				(g:emit (list 'BNZ (list 'code l2)))
				(g:texpr (g:third e))
				(g:label! l2)
				(set! g:ty g:INT))
		(if (= h 'comma) (g:comma (tail e))
			(begin
				(define b (g:assoc/atom h g:binops))
				(if b (g:binary e (g:second b))
				(g:die (+ "bad expression node: " (+ "" h))))))))))))))))))))))))))))))))))))))))))

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
				(g:emit (list 'IMM (g:glooperand (index s 3))))
				(g:varload s))
		(if (= cl 'extg)
			(begin
				(g:emit (list 'IMM (list 'extph (index s 3))))
				(g:varload s))
		(if (if (= cl 'fun) true (= cl 'ext))
			(begin
				;; function name as a value: its address
				(g:emit (list 'IMM (g:funref s)))
				(set! g:ty (g:vartype s))
				(set! g:lastarray false))
		(g:die (+ "bad variable: " n)))))))))

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

;; c4's ?: has its else-arm's type. With an unsigned arm in it, both
;; arms convert to the usual arithmetic type instead, as C says.
(define g:condexpr (lambda (e)
	(begin
		(define tt (g:typeof (g:third e)))
		(define et (g:typeof (index e 3)))
		(define ll (g:llop? false tt et))
		(define u (if ll false (g:unsbin? tt et)))
		(define rt (g:uac tt et))
		(g:texpr (g:second e))
		(define l1 (g:newlabel))
		(define l2 (g:newlabel))
		(g:emit (list 'BZ (list 'code l1)))
		(if ll (g:llval (g:third e)) (g:expr (g:third e)))
		(if u (g:convert! g:ty rt) nil)
		(g:emit (list 'JMP (list 'code l2)))
		(g:label! l1)
		(if ll (g:llval (index e 3)) (g:expr (index e 3)))
		(if u (begin (g:convert! g:ty rt) (set! g:ty rt)) nil)
		(if ll (set! g:ty rt) nil)
		(g:label! l2))))

;; ---- statements ----

;; frame words needed by declstmt nodes anywhere under a statement
;; (ENT must reserve the whole frame up front)
(define g:scanstmt (lambda (s)
	(begin
		(define h (head s))
		(if (= h 'line) (g:scanstmt (g:third s))
		(if (= h 'declstmt) (g:scandecls (tail s) 0)
		(if (= h 'block) (g:scanlist (tail s) 0)
		(if (= h 'if)
			(+ (g:scanstmt (g:third s))
				(if (= (index s 3) nil) 0 (g:scanstmt (index s 3))))
		(if (= h 'while) (g:scanstmt (g:third s))
		(if (= h 'dowhile) (g:scanstmt (g:second s))
		(if (= h 'for) (g:scanstmt (index s 4))
		(if (= h 'switch) (g:scanlist (tail (tail s)) 0)
		(if (= h 'labeled) (g:scanstmt (g:third s))
		0))))))))))))
(define g:scanlist (lambda (l n)
	(if (empty? l) n
	(next g:scanlist (tail l) (+ n (g:scanstmt (head l)))))))
(define g:scandecls (lambda (ls n)
	(if (empty? ls) n
	(next g:scandecls (tail ls) (+ n (g:lwords (head ls)))))))

(define g:lcur 0)   ;; frame slot cursor for block-scoped declarations

(define g:stmt (lambda (s)
	(begin
		(define ts g:tcur)
		(g:stmt/1 (if g:fpon (g:fpstmt s) s))
		(set! g:tcur ts))))
(define g:stmt/1 (lambda (s)
	(begin
		(define h (head s))
		;; -g: a line item, the zero-size mark the source map's rows are
		;; made from (c4r:encode), then the statement
		(if (= h 'line)
			(begin
				(g:emit (list 'line (g:second s)))
				(set! g:lineloc (g:second s))
				(g:stmt (g:third s)))
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
				(if (= (g:second s) nil) nil				(if (g:sret? g:curret)
				(if (g:ll? g:curret)
					;; through the caller's pointer, the last argument
					(begin
						(g:emit '(LEA 2)) (g:emit '(LI)) (g:emit '(PSH))
						(g:llval (g:second s))
						(g:llcopy))
					;; a struct, the same way (M12): its bytes copied there,
					;; and the pointer left in A, MCPY's result
					(begin
						(g:emit '(LEA 2)) (g:emit '(LI)) (g:emit '(PSH))
						(g:sval (g:second s) g:curret "a return")
						(g:emit '(PSH)) (g:emit (list 'IMM (g:tysize g:curret))) (g:emit '(PSH))
						(g:emit '(MCPY)) (g:emit '(ADJ 3))))
					(begin
						(g:expr (g:second s))
						(g:sconv! g:ty g:curret)
						(if (g:isptr g:curret)							(if (g:dropped? g:curretq (g:qualof (g:second s)))
								(g:qwarn "return") nil)
						nil))))
				(g:emit '(LEV)))
		(if (= h 'break)
			(if (= g:brk nil) (g:die "break outside of loop or switch")
				(g:emit (list 'JMP (list 'code g:brk))))
		(if (= h 'continue)
			(if (= g:cont nil) (g:die "continue outside of loop")
				(g:emit (list 'JMP (list 'code g:cont))))
		(if (= h 'empty) nil
		;; T4b: only the inliner makes these; the parser never does.
		(if (= h 'ijmp)
			(g:emit (list 'JMP (list 'code (g:ilab (g:second s)))))
		(if (= h 'ilabel) (g:label! (g:ilab (g:second s)))
		(if (= h 'goto)
			(begin
				(set! g:uwanted (g:cons (g:second s) g:uwanted))
				(g:emit (list 'JMP (list 'code (g:ulab (g:second s))))))
		(if (= h 'labeled)
			(begin
				(g:uplace! (g:second s))
				(g:stmt (g:third s)))
		(if (= h 'switch) (g:switchstmt s)
		(g:die (+ "bad statement node: " (+ "" h)))))))))))))))))))))))
(define g:stmts (lambda (l)
	(if (empty? l) nil
	(begin
		(g:stmt (head l))
		(next g:stmts (tail l))))))

(define g:ifstmt (lambda (s)
	(begin
		(g:texpr (g:second s))
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
		(g:texpr (g:second s))
		(g:emit (list 'BZ (list 'code lend)))
		(define here g:lineloc)
		(g:stmt (g:third s))
		(g:loopline here)
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
			(g:texpr (g:third s))
			(g:emit (list 'BZ (list 'code lend)))))
		(define here g:lineloc)
		(g:stmt (index s 4))
		(g:loopline here)
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
		;; a span past the word (INT_MIN to INT_MAX on a 32-bit host) wraps
		;; below zero, and is sparse too
		(if (if (< range 0) true (>= range g:SWRANGE)) (g:swchain cases 0 (if (= def nil) lend def))
		(g:swtabled cases def lend mn range))))))
;; a switch whose cases span g:SWRANGE or more: compares, in the order the
;; cases are written. a holds the value, and stays the value XORed with
;; the case at hand -- zero when it matches -- so nothing is pushed that a
;; branch would have to drop:
;;
;;     PSH; IMM c1;       XOR; BZ case1
;;     PSH; IMM c1 ^ c2;  XOR; BZ case2      a = value ^ c2
;;     ...
;;     JMP default-or-end
(define g:swchain (lambda (cases prev defloc)
	(if (empty? cases) (g:emit (list 'JMP (list 'code defloc)))
	(begin
		(define v (head (head cases)))
		(g:emit '(PSH))
		(g:emit (list 'IMM (bit:xor v prev)))
		(g:emit '(XOR))
		(g:emit (list 'BZ (list 'code (g:second (head cases)))))
		(next g:swchain (tail cases) v defloc)))))
(define g:swtabled (lambda (cases def lend mn range)
	(begin
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
		(g:emit (list 'JMP (list 'code defloc))))))

(define g:switchstmt (lambda (s)
	(begin
		(g:iexpr (g:second s))
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

;; struct layouts first of all: a parameter passed by value takes its
;; struct's words, which the function passes below must count (M9)
(define g:structpass (lambda (decls)
	(if (empty? decls) nil
	(begin
		(if (= (head (head decls)) 'structdef) (g:structdef (head decls)) nil)
		(next g:structpass (tail decls))))))
;; pre-pass: register every defined function so forward calls resolve
(define g:prepass (lambda (decls)
	(if (empty? decls) nil
	(begin
		(define d (head decls))
		(if (g:helperdef? d)
			(begin
				(if (= (g:third d) "__c4f_add") (set! g:fpon true) nil)
				(set! g:llsrc (g:cons (list (g:third d) d) g:llsrc)))
		(if (= (head d) 'func)
			(set! g:syms (g:cons
				(list (g:third d) 'fun (g:second d) (g:newlabel)
					;; ArgCount includes the variadic fake slot, as c4cc
					(+ (g:pwords (index d 3) 0) (if (index d 4) 1 0))
					(index d 4)
					(index d 5)    ;; declaration attrs (static etc.)
					;; M4: return mask, parameter types and masks
					(g:nz (index d 8)) (g:ptys (index d 3) (list)) (g:pqs (index d 3) (list)))
				g:syms))
			nil))
		(next g:prepass (tail decls))))))
(define g:ptys (lambda (ps acc)
	(if (empty? ps) (g:reverse acc)
	(next g:ptys (tail ps) (g:cons (head (head ps)) acc)))))
(define g:pqs (lambda (ps acc)
	(if (empty? ps) (g:reverse acc)
	(next g:pqs (tail ps) (g:cons (g:nz (index (head ps) 2)) acc)))))

;; second pre-pass: prototypes with no definition anywhere in the
;; unit. In OBJECT MODE (-c) they become extern symbols: calls carry
;; SYMBOL-typed patches for c4rlink to resolve. In whole-program mode
;; they get a stub that exits with 255 if ever reached -- c4cc
;; "supports" these by emitting a call through an unresolved extern
;; (which jumps to garbage); the stub is the deterministic version of
;; the same contract: legal to declare and call-compile, fatal to
;; execute.
(define gen:objmode false)
;; M12, -mcisc: emit c4mp-only fused array-element opcodes (LXI/SXI)
;; instead of the generic index codegen, for int/pointer-element
;; (8-byte) arrays -- see g:indexload-cisc/g:indexstore-cisc below and
;; docs/c4or1k-design.md's M12 section. Off by default; code compiled
;; with it must run under c4mp, not c4m (c4m traps the new opcodes).
(define gen:cisc false)
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
							(+ (g:pwords (index d 3) 0) (if (index d 4) 1 0))
							(index d 4)
							nil (g:nz (index d 6)) (index d 3) (index d 7))
						g:syms))
					(set! g:externs (g:cons
						(list (g:third d) (g:second d) (index d 4) 129 nil)
						g:externs))
					(set! g:nexterns (+ g:nexterns 1)))
			(begin
				(define l (g:newlabel))
				(set! g:syms (g:cons
					(list (g:third d) 'fun (g:second d) l
						(+ (g:pwords (index d 3) 0) (if (index d 4) 1 0))
						(index d 4)
						16    ;; ATTR_EXTERN, as c4cc marks prototypes
						(g:nz (index d 6)) (index d 3) (index d 7))
					g:syms))
				(set! g:stubs (g:cons l g:stubs))))
			nil)
		(next g:protopass (tail decls))))))
(define g:emitstubs (lambda (ls)
	(if (empty? ls) nil
	(begin
		(g:madebody "<stub>")
		(g:label! (head ls))
		(g:emit '(ENT 0))
		(g:emit '(IMM 255))
		(g:emit '(PSH))
		(g:emit '(EXIT))
		(next g:emitstubs (tail ls))))))

;; third pre-pass: struct layouts and global storage, in declaration
;; order (so relative data layout is what the old in-walk allocation
;; produced), before any function body is generated. That makes
;; extern-then-define and define-after-use both work: by the time a
;; body references a global, every definition in the unit is
;; registered. Extern declarations with no in-unit definition are
;; collected and become extern DATA symbols in object mode -- class
;; Glo, ATTR_EXTERN, referenced through the same (extph K)
;; placeholders as extern functions, resolved by c4rlink to DATA
;; patches. Whole-program mode has nowhere to resolve them: fatal.
(define g:datapass (lambda (decls pending)
	(if (empty? decls) (g:extdata pending)
	(begin
		(define d (head decls))
		(define isglobal (= (head d) 'global))
		(define isext (if isglobal
			(if (= 16 (bit:and (index d 4) 16)) (= (index d 5) nil) false)
			false))
		(if (= (head d) 'structdef) nil      ;; g:structpass did it
		(if isglobal (if isext nil
			(if (if (= (index d 5) nil) (g:laterinit? (g:third d) (tail decls)) false) nil
			(g:global d)))
		nil))
		(next g:datapass (tail decls)
			(if isext (+ pending (list d)) pending))))))

;; `int x;` before `int x = 9;` is one object, the definition's: the
;; first allocates nothing (M7b; c4fc's DECL retires its record)
(define g:laterinit? (lambda (n ds)
	(if (empty? ds) false
	(if (if (= (head (head ds)) 'global)
			(if (= (g:third (head ds)) n) (if (= (index (head ds) 5) nil) false true) false)
			false)
		true
	(next g:laterinit? n (tail ds))))))

;; register the leftover extern data declarations (skipping any name
;; the unit defined after all); ARR bytes follow g:globalarray's
;; sizing so indexing and sizeof agree with the defining unit
(define g:extdata (lambda (ds)
	(if (empty? ds) nil
	(begin
		(define d (head ds))
		(define n (g:third d))
		(if (g:lookup n g:syms) nil
		(if gen:objmode
			(begin
				(define ty (g:second d))
				(define size (index d 3))
				(define arrbytes
					(if (if (= size nil) (g:svalue? ty) false) (g:tysize ty)
					(if (= size nil) nil
						(* size (g:estride (- ty g:PTR))))))
				(set! g:syms (g:cons
					(list n 'extg ty g:nexterns arrbytes (g:nz (index d 6))) g:syms))
				(set! g:externs (g:cons
					(list n ty nil 131 arrbytes) g:externs))
				(set! g:nexterns (+ g:nexterns 1)))
			;; whole-program: nothing defines it, and nothing may use it --
			;; a name no symbol answers is an error where it is used. Only
			;; declaring it is fine, as in C (CASSERT's array, M9)
			nil))
		(next g:extdata (tail ds))))))

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
		(if (= bytewise 2)
			(begin
				(string:byte! g:data g:dlen (bit:and v 255))
				(string:byte! g:data (+ g:dlen 1) (bit:and (bit:shr v 8) 255))
				(set! g:dlen (+ g:dlen 2)))
		(if bytewise
			(begin
				(string:byte! g:data g:dlen v)
				(set! g:dlen (+ g:dlen 1)))
			(g:dword v)))
		(next g:delems (if (empty? vs) vs (tail vs)) (- s 1) bytewise)))))

;; the qualifier mask of the global being allocated (a mailbox, so the
;; three allocators keep their signatures)
(define g:gq 0)

;; global declaration: scalar word (c4cc gives char globals a whole
;; word too) or array storage; "str" and &fn initializers add
;; data-resident patches
(define g:global (lambda (d)
	(begin
		(define ty (g:second d))
		(define n (g:third d))
		(define size (index d 3))
		(define init (index d 5))
		;; "extern int x = 5" is a definition: storage is allocated
		;; here, so strip ATTR_EXTERN -- to c4rlink that bit means
		;; UNDEFINED and the symbol would never resolve
		(define ga0 (index d 4))
		(define gattrs
			(if (= 16 (bit:and ga0 16)) (- ga0 16) ga0))
		(set! g:gq (g:nz (index d 6)))
		(if (if (= size nil) (g:svalue? ty) false)
			(g:globalstruct n ty init gattrs)
		(if (= size nil)
			(g:globalscalar n ty init gattrs)
		(g:globalarray n ty size init gattrs))))))

;; struct-valued global: zeroed storage, name evaluates to its address.
;; With an initializer it is data, laid out by g:globalaggr (M7b).
(define g:globalstruct (lambda (n ty init gattrs)
	(if (= init nil) (g:globalstruct/bss n ty gattrs)
	(g:globalaggr n ty nil init gattrs))))
(define g:globalstruct/bss (lambda (n ty gattrs)
	(begin
		(define bytes (g:tysize ty))
		;; a struct global is never initialized -> pure BSS
		(set! g:syms (g:cons (list n 'glo ty (g:balloc bytes) bytes gattrs g:gq) g:syms)))))

(define g:globalscalar (lambda (n ty init0 gattrs)
	(begin
		(define init (g:fpinit init0 ty))
		(define at
			;; no initializer -> BSS (one word); otherwise placed in data
			(if (= init nil) (g:balloc (if (g:ll? ty) 8 g:WORD))
			(if (if (= (head init) 'num) (g:ll? ty) false)
				(begin
					(define lo (g:dword (g:second init)))
					(g:dword (if (< (g:second init) 0) -1 0))
					lo)
			(if (= (head init) 'llnum)
				(begin
					(define lo2 (g:dword (g:second init)))
					(g:dword (g:third init))
					lo2)
			(if (= (head init) 'num) (g:dword (g:second init))
			(if (= (head init) 'str)
				(begin
					(define soff (g:dstr (g:second init)))
					(define slot (g:dword 0))
					(set! g:dpatches (g:cons (list 'ddata slot soff) g:dpatches))
					slot)
			(if (if (= (head init) 'fnaddr) true (= (head init) 'name))
				(begin
					(define f (g:lookup (g:second init) g:syms))
					(if (if f (g:addressable? f) true) nil
						(g:die "& initializer requires a defined function or a global"))
					(define slot (g:dword 0))
					(set! g:dpatches (g:cons (g:addrpatch slot (g:second init) f) g:dpatches))
					slot)
			(g:die (+ "unsupported global initializer for " n)))))))))
		(set! g:syms (g:cons (list n 'glo ty at nil gattrs g:gq) g:syms)))))

;; array storage: char arrays are s bytes, everything else s words;
;; uninitialized/short-initialized elements are zero
(define g:globalarray (lambda (n ty s init gattrs)
	(begin
		(define elem (- ty g:PTR))
		(define bytewise (if (g:bytety? elem) true (if (g:halfty? elem) 2 false)))
		(define bytes (* s (g:estride elem)))
		;; an uninitialized array is pure BSS (occupies no image bytes,
		;; does not touch g:data/DMAX -- so it may be arbitrarily large)
		(if (= init nil)
			(set! g:syms (g:cons (list n 'glo ty (g:balloc bytes) bytes gattrs g:gq) g:syms))
			(g:globalarrayinit n ty s init gattrs elem bytewise bytes)))))

;; the initialized-array case (kept out of g:globalarray so its own BSS
;; branch stays a simple one-liner). Storage goes in the data segment.
;; Anything but plain numbers or a string, or any element that is a
;; struct, is g:globalaggr's (M7b).
(define g:globalarrayinit (lambda (n ty s init gattrs elem bytewise bytes)
	(if (if (= (head init) 'agg) true (if (g:svalue? elem) true (if (g:ll? elem) true
			(if (g:rowty? elem) true (g:fp? elem)))))
		(g:globalaggr n ty s init gattrs)
	(g:globalarrayinit/2 n ty s init gattrs elem bytewise bytes))))
(define g:globalarrayinit/2 (lambda (n ty s init gattrs elem bytewise bytes)
	(begin
		(g:dalign)
		(define at g:dlen)
		(if (> (+ at bytes) g:DMAX) (g:die "data segment full") nil)
		(if (= (head init) 'braces)
			(begin
				(if (g:svalue? elem)
					(g:die (+ "struct arrays cannot be initialized: " n)) nil)
				(if (> (length (g:second init)) s)
					(g:die (+ "too many initializers for " n)) nil)
				(g:delems (g:second init) s bytewise))
		(if (if (= (head init) 'str) (= bytewise true) false)
			(begin				;; C drops the NUL of a string that fills the array exactly (M13)
				(if (< s (length (g:second init)))
					(g:die (+ "string does not fit the array " n)) nil)
				(g:dstrbytes (g:second init) 0)
				(g:dzero (- s (length (g:second init)))))
		(g:die (+ "unsupported array initializer for " n))))
		(set! g:syms (g:cons (list n 'glo ty at bytes gattrs g:gq) g:syms)))))
(define g:dstrbytes (lambda (str i)
	(if (>= i (length str)) nil
	(begin
		(string:byte! g:data g:dlen (string:byte str i))
		(set! g:dlen (+ g:dlen 1))
		(next g:dstrbytes str (+ i 1))))))

;; ---- aggregate initializers (M7b) ----
;;
;; A global array or struct whose initializer is more than numbers: strings,
;; function addresses, nested braces, structs, `[i] =` and `.m =`. The
;; initializer is walked against the type twice, the same way both times:
;;
;;   'str: every string a POINTER takes goes into the data now, in order,
;;         before the global -- as `char *s = "..."` has always put it;
;;   'put: the global's bytes are reserved, zeroed, and each value written
;;         at its offset; a string pointer and a function address become a
;;         data patch where they lie, in the order they lie.
;;
;; A value is a byte, two bytes or a word by its type, as g:delems writes
;; them. An aggregate met without braces takes its elements from the
;; enclosing list (C's brace elision). c4fc's AGG-* words are the same.
(define g:imode 'put)
(define g:istrs (list))
(define g:globalaggr (lambda (n ty cnt init gattrs)
	(begin
		(define ety (if (= cnt nil) ty (- ty g:PTR)))
		(define bytes (if (= cnt nil) (g:tysize ty) (* cnt (g:estride ety))))
		(define items (g:items init))
		(set! g:imode 'str)
		(set! g:istrs (list))
		(g:ifillagg ety cnt 0 items true)
		(set! g:istrs (g:reverse g:istrs))
		(g:dalign)
		(define at g:dlen)
		(if (> (+ at bytes) g:DMAX) (g:die "data segment full") nil)
		(g:dzero bytes)
		(set! g:imode 'put)
		(g:ifillagg ety cnt at items true)
		(set! g:syms (g:cons (list n 'glo ty at bytes gattrs g:gq) g:syms)))))
;; an initializer's items: a brace list's elements, or the one initializer
(define g:items (lambda (it)
	(if (= (head it) 'agg) (g:second it)
	(if (= (head it) 'braces) (g:numitems (g:second it) (list))
	(list it)))))
(define g:numitems (lambda (l acc)
	(if (empty? l) (g:reverse acc)
	(next g:numitems (tail l) (g:cons (list 'num (head l)) acc)))))
;; one object of element type ETY (an array of CNT of them when CNT is
;; not nil) at OFF, from the cursor CUR; returns what is left of CUR
(define g:ifill (lambda (ety cnt off cur)
	(if (if (= cnt nil) (g:rowty? ety) false)
		(next g:ifill (g:rowelem ety) (g:rowcount ety) off cur)
	(g:ifill/2 ety cnt off cur))))
(define g:ifill/2 (lambda (ety cnt off cur)
	(begin
		(define it (head cur))
		(define h (head it))
		(define agg (if (= cnt nil) (g:svalue? ety) true))
		(if (= agg false)
			(begin (g:iscalar ety off (g:unwrap it)) (tail cur))
		(if (if (= h 'agg) true (= h 'braces))
			(begin (g:ifillagg ety cnt off (g:items it) true) (tail cur))
		(if (if (= h 'str) (if (= cnt nil) false (g:bytety? ety)) false)
			(begin (g:istrfill cnt off (g:second it)) (tail cur))
		(g:ifillagg ety cnt off cur false)))))))
;; `{ 5 }` for a scalar is 5
(define g:unwrap (lambda (it)
	(if (if (= (head it) 'agg) true (= (head it) 'braces))
		(begin
			(define l (g:items it))
			(if (if (empty? l) false (empty? (tail l))) (g:unwrap (head l))
			(g:die "too many initializers for a scalar")))
	it)))
;; an aggregate's elements from ITEMS; BRACED is false when they come
;; from an enclosing list, which keeps what this one does not take
(define g:ifillagg (lambda (ety cnt off items braced)
	(if (= cnt nil) (g:ifillstruct ety off items braced)
	(g:ifillarr ety cnt off items braced 0))))
(define g:ifillarr (lambda (ety cnt off items braced idx)
	(if (empty? items) items
	(begin
		(define it (head items))
		(if (= (head it) 'desig)
			(if braced
				(next g:ifillarr ety cnt off (g:cons (g:third it) (tail items)) braced (g:second it))
			items)
		(if (>= idx cnt)
			(if braced (g:die "too many initializers for an array") items)
		(next g:ifillarr ety cnt off
			(g:ifill ety nil (+ off (* idx (g:estride ety))) items)
			braced (+ idx 1))))))))
(define g:ifillstruct (lambda (sty off items braced)
	(begin
		(define ms (g:third (g:sinfo sty)))
		(g:ifillst ms ms off items braced (g:union? ms)))))
(define g:ifillst (lambda (all ms off items braced uni)
	(if (empty? items) items
	(begin
		(define it (head items))
		(if (= (head it) 'mdesig)
			(if braced
				(next g:ifillst all (g:memfrom all (g:second it)) off
					(g:cons (g:third it) (tail items)) braced uni)
			items)
		(if (empty? ms)
			(if braced (g:die "too many initializers for a struct") items)
		(begin
			(define m (head ms))       ;; (NAME OFF TYPE BYTES Q)
			(define mty (index m 2))
			(define rest
				(if (if (g:isptr mty) (if (= (index m 3) nil) false true) false)
					(g:ifill (- mty g:PTR) (/ (index m 3) (g:estride (- mty g:PTR)))
						(+ off (g:second m)) items)
				(g:ifill mty nil (+ off (g:second m)) items)))
			(next g:ifillst all (if uni (list) (tail ms)) off rest braced uni))))))))
;; a union lays every member at 0; it takes one initializer
(define g:union? (lambda (ms)
	(if (empty? ms) false (if (empty? (tail ms)) false (g:allzero? ms)))))
(define g:allzero? (lambda (ms)
	(if (empty? ms) true (if (= (g:second (head ms)) 0) (next g:allzero? (tail ms)) false))))
(define g:memfrom (lambda (ms n)
	(if (empty? ms) (g:die (+ "no such member in a designator: " n))
	(if (= (head (head ms)) n) ms (next g:memfrom (tail ms) n)))))
;; a char array's string: its bytes; the rest stays zero
(define g:istrfill (lambda (cnt off s)
	(begin
		(if (> (length s) cnt) (g:die "a string does not fit its array") nil)
		(if (= g:imode 'put) (g:istrfill/2 s 0 off)
		(if (= g:imode 'local) (g:lstrfill s 0 off) nil)))))
(define g:istrfill/2 (lambda (s i off)
	(if (>= i (length s)) nil
	(begin
		(string:byte! g:data (+ off i) (string:byte s i))
		(next g:istrfill/2 s (+ i 1) off)))))
(define g:iscalar (lambda (ty off it)
	(if (= g:imode 'local) (g:lstore ty off it)
	(g:iscalar/g ty off it))))
(define g:iscalar/g (lambda (ty off it0)
	(begin
		(define it (g:fpinit it0 ty))
		(define h (head it))
		(if (= h 'llnum)
			(if (= g:imode 'put)
				(begin
					(string:word! g:data off (g:second it))
					(string:word! g:data (+ off g:WORD) (g:third it)))
			nil)
		(if (= h 'num) (if (= g:imode 'put) (g:iput ty off (g:second it)) nil)
		(if (= h 'str)
			(if (= g:imode 'str)
				(set! g:istrs (g:cons (g:dstr (g:second it)) g:istrs))
			(begin
				(set! g:dpatches (g:cons (list 'ddata off (head g:istrs)) g:dpatches))
				(set! g:istrs (tail g:istrs))))
		(if (if (= h 'name) true (= h 'fnaddr))
			(begin
				(define f (g:lookup (g:second it) g:syms))
				(if (if f (g:addressable? f) true) nil
					(g:die (+ "an initializer can name only a function or a global: " (g:second it))))
				(if (= g:imode 'put)
					(set! g:dpatches (g:cons (g:addrpatch off (g:second it) f) g:dpatches)) nil))
		;; a global's address plus an offset, &a[k] (p:foldinit): a data
		;; patch carrying the offset
		(if (= h 'gaddr)
			(begin
				(define gf (g:lookup (g:second it) g:syms))
				(if (if gf (if (= (g:second gf) 'fun) true (= (g:second gf) 'ext)) false)
					(g:die (+ "an offset from a function's address: " (g:second it))) nil)
				(if (= g:imode 'put)
					(set! g:dpatches (g:cons (list 'dglo off (g:second it) (g:third it)) g:dpatches)) nil))
		;; a string's address plus an offset
		(if (= h 'sadr)
			(if (= g:imode 'str)
				(set! g:istrs (g:cons (g:dstr (g:second it)) g:istrs))
			(begin
				(set! g:dpatches (g:cons (list 'ddata off (+ (head g:istrs) (g:third it))) g:dpatches))
				(set! g:istrs (tail g:istrs))))
		(g:die "unsupported initializer element"))))))))))
;; a data word holding function F's address: a code patch for a function
;; this unit defines, and for an extern one (-c) a symbol patch whose slot
;; is in data (type -5), which c4rlink resolves when it links the unit
;; that defines it -- Busybox's applet_main[] names every applet's main.
;; A global's address (M7b) is (dglo SLOT NAME) until the end of the
;; unit, when g:resolvedglo knows where the bss lands and which names are
;; extern data -- g:extdata registers those last, so a name not known yet
;; is one: a data-to-data patch for a global, and a symbol patch like an
;; extern function's for extern data.
(define g:addressable? (lambda (f)
	(if (= (g:second f) 'fun) true (if (= (g:second f) 'ext) true
	(if (= (g:second f) 'glo) true (= (g:second f) 'extg))))))
(define g:fnpatch (lambda (slot f)
	(if (= (g:second f) 'ext) (list 'dsym slot (index f 3))
	(list 'dcode slot (index f 3)))))
(define g:addrpatch (lambda (slot n f)
	(if (if f (if (= (g:second f) 'fun) true (= (g:second f) 'ext)) false)
		(g:fnpatch slot f)
	(list 'dglo slot n))))
(define g:resolvedglo (lambda (ps base acc)
	(if (empty? ps) (g:reverse acc)
	(begin
		(define p (head ps))
		(next g:resolvedglo (tail ps) base (g:cons
			(if (= (head p) 'dglo) (g:resolvedglo/1 p base) p)
			acc))))))
(define g:resolvedglo/1 (lambda (p base)
	(begin
		(define s (g:lookup (g:third p) g:syms))
		(if s nil (g:die (+ "an initializer names no function or global: " (g:third p))))
		(define off (index s 3))
		;; &a[k]: the offset rides in the patch, and for extern data in the
		;; word itself, which c4rlink adds to the symbol's address
		(define k (if (> (length p) 3) (index p 3) 0))
		(if (= (g:second s) 'extg)
			(begin
				(if (= k 0) nil (string:word! g:data (g:second p) k))
				(list 'dsym (g:second p) off))
		(list 'ddata (g:second p) (+ k (if (g:bss? off) (+ base (g:bssoff off)) off)))))))
(define g:remapdsym (lambda (ps base acc)
	(if (empty? ps) (g:reverse acc)
	(begin
		(define p (head ps))
		(next g:remapdsym (tail ps) base (g:cons
			(if (= (head p) 'dsym) (list 'dsym (g:second p) (+ base (g:third p))) p)
			acc))))))
(define g:iput (lambda (ty off v)
	(if (g:ll? ty)
		(begin
			(string:word! g:data off v)
			(string:word! g:data (+ off g:WORD) (if (< v 0) -1 0)))
	(if (g:bytety? ty) (string:byte! g:data off (bit:and v 255))
	(if (g:halfty? ty)
		(begin
			(string:byte! g:data off (bit:and v 255))
			(string:byte! g:data (+ off 1) (bit:and (bit:shr v 8) 255)))
	(string:word! g:data off v))))))

;; ---- a local aggregate's initializer (M7b) ----
;;
;; The walk above in a third mode, 'local: the local is zeroed with MSET
;; (memset's opcode, which needs no libc), then each value is stored at
;; its offset as the declaration runs -- a number, a string's address, or
;; any expression (p:linit's (expr E)). A word-aligned offset is LEA's
;; own; any other is added to the base. c4fc's n_lagg is the same.
(define g:lbase 0)
(define g:localaggr (lambda (d val)
	(begin
		(define size (index d 3))
		(define ty (g:second d))
		(set! g:lbase (- g:loc val))
		(g:emit (list 'LEA g:lbase))
		(g:emit '(PSH))
		(g:emit '(IMM 0))
		(g:emit '(PSH))
		(g:emit (list 'IMM (g:lbytes d)))
		(g:emit '(PSH))
		(g:emit '(MSET))
		(g:emit '(ADJ 3))
		(set! g:imode 'local)
		(g:ifillagg (if (= size nil) ty (- ty g:PTR)) size 0 (g:items (index d 4)) true)
		(set! g:imode 'put))))
(define g:laddr (lambda (off)
	(if (= 0 (bit:and off g:WMASK)) (g:emit (list 'LEA (+ g:lbase (/ off g:WORD))))
	(begin
		(g:emit (list 'LEA g:lbase))
		(g:emit '(PSH))
		(g:emit (list 'IMM off))
		(g:emit '(ADD))))))
(define g:lstore (lambda (ty off it)
	(if (g:fp? ty) (g:fplstore ty off it)
	(if (g:ll? ty)
		(begin
			(g:laddr off)
			(g:emit '(PSH))
			(g:llval (if (= (head it) 'expr) (g:second it) it))
			(g:llcopy))
	(g:lstore/2 ty off it)))))
(define g:lstore/2 (lambda (ty off it)
	(begin
		(define h (head it))
		(g:laddr off)
		(g:emit '(PSH))
		(if (= h 'num) (g:emit (list 'IMM (g:second it)))
		(if (= h 'str) (g:expr it)
		(if (= h 'expr) (g:expr (g:second it))
		(g:die "unsupported initializer element"))))
		(if (if (= h 'num) false (g:svalue? g:ty))
			(g:die "a struct value in an initializer is not supported") nil)
		(g:store! ty))))
(define g:lstrfill (lambda (s i off)
	(if (>= i (length s)) nil
	(begin
		(g:lstore g:CHAR (+ off i) (list 'num (string:byte s i)))
		(next g:lstrfill s (+ i 1) off)))))

;; the words one parameter of type ty takes: two for a long long, a
;; struct's own size for a struct passed by value, else one
(define g:pw (lambda (ty)
	(if (g:ll? ty) 2
	(if (g:svalue? ty) (/ (+ (g:tysize ty) g:WMASK) g:WORD)
	1))))
;; locals: params idx 0..argc-1, loc = argc+1, scalars idx loc+1...;
;; initializer stores re-run on every entry, emitted after ENT
(define g:params (lambda (ps i)
	(if (empty? ps) i
	(begin
		;; several words: the last pushed first, so the first is at the
		;; lowest address, which is the parameter's
		(define w (g:pw (head (head ps))))
		(set! g:syms (g:cons
			;; a struct's is its storage, as a local struct's is
			(list (g:second (head ps)) 'loc (head (head ps)) (+ i (- w 1))
				(if (g:svalue? (head (head ps))) (g:tysize (head (head ps))) nil)
				(g:nz (index (head ps) 2)))
			g:syms))
		(next g:params (tail ps) (+ i w))))))
;; the words a parameter list takes
(define g:pwords (lambda (ps n)
	(if (empty? ps) n
	(next g:pwords (tail ps) (+ n (g:pw (head (head ps))))))))

;; local slots, c4cc's exact scheme: i += words consumed, the symbol's
;; Val is i AFTER the add, so LEA (loc - Val) is the LOWEST slot and
;; array indexing ascends. char arrays pack bytes into whole words.
;; frame words one declaration needs (struct-aware)
(define g:lwords (lambda (d)
	(begin
		(define size (index d 3))
		(define ty (g:second d))
		(if (= size nil)
			(if (g:svalue? ty) (/ (+ (g:tysize ty) g:WMASK) g:WORD) (if (g:ll? ty) 2 1))
		(g:awords size (- ty g:PTR))))))
;; its sizeof value in bytes, nil for plain scalars
(define g:lbytes (lambda (d)
	(begin
		(define size (index d 3))
		(define ty (g:second d))
		(if (= size nil)
			(if (g:svalue? ty) (g:tysize ty) nil)
		(* size (g:estride (- ty g:PTR)))))))

(define g:localdefs (lambda (ls i)
	(if (empty? ls) i
	(begin
		(define d (head ls))   ;; (local TYPE NAME SIZE INIT [Q])
		(define words (g:lwords d))
		(set! g:syms (g:cons
			(list (g:third d) 'loc (g:second d) (+ i words) (g:lbytes d) (g:nz (index d 5)))
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

;; a 16-bit element is two byte stores, low byte first
(define g:linit/half (lambda (val idx v)
	(begin
		(g:linit/byte val (* 2 idx) (bit:and v 255))
		(g:linit/byte val (+ (* 2 idx) 1) (bit:and (bit:shr v 8) 255)))))

;; mode: true = bytes, 2 = 16-bit halves, false = words
(define g:linit/elems (lambda (val vs s idx bytewise)
	(if (>= idx s) nil
	(begin
		(define v (if (empty? vs) 0 (head vs)))
		(if (= bytewise 2) (g:linit/half val idx v)
		(if bytewise (g:linit/byte val idx v) (g:linit/word val idx v)))
		(next g:linit/elems val (if (empty? vs) vs (tail vs)) s (+ idx 1) bytewise)))))

(define g:strbytes (lambda (str i acc)   ;; string -> list of byte values
	(if (>= i (length str)) (g:reverse acc)
	(next g:strbytes str (+ i 1) (g:cons (string:byte str i) acc)))))

;; A scalar local's initializer with a float or double on one side, and
;; not both of one type, is converted as an assignment converts: to an
;; integer through a cast, to a float by (fpinit E), which eminit emits
;; with fpval. Anything else is left alone.
(define g:fpinit/conv (lambda (init ty size)
	(if (if (= init nil) true (if (not (= size nil)) true (not (= (head init) 'einit)))) init
	(begin
		(define et (g:typeof (g:second init)))
		(if (= et ty) init
		(if (g:fp? ty) (list 'fpinit (g:second init))
		(if (g:fp? et) (list 'einit (list 'cast ty (g:second init)))
		init)))))))

;; emit the initializer of local node d whose frame slot is val
(define g:eminit (lambda (d val)
	(begin
		(define init (g:fpinit/conv (index d 4) (g:second d) (index d 3)))
		(define size (index d 3))
		(define bytewise
			(if (= size nil) false
			(if (g:bytety? (- (g:second d) g:PTR)) true
			(if (g:halfty? (- (g:second d) g:PTR)) 2
			false))))		(if (= init nil) nil
		(if (= (head init) 'fpinit)
			(begin
				(g:emit (list 'LEA (- g:loc val)))
				(g:emit '(PSH))
				(g:fpval (g:second init) (g:second d))
				(g:store! (g:second d)))
		(if (= size nil)
			;; scalar: a full initializer expression, stored as a
			;; word (c4cc used SI even for char scalars; the narrow types
			;; read their low bytes back, and a uint load masks)
			(if (if (= (head init) 'einit) (g:ll? (g:second d)) false)
				(begin
					(g:emit (list 'LEA (- g:loc val)))
					(g:emit '(PSH))
					(g:llval (g:second init))
					(g:llcopy))			(if (= (head init) 'einit)
				(if (g:svalue? (g:second d))
					;; from a struct value (M12): its bytes
					(begin
						(g:emit (list 'LEA (- g:loc val)))
						(g:emit '(PSH))
						(g:sval (g:second init) (g:second d) "an initializer")
						(g:emit '(PSH)) (g:emit (list 'IMM (g:tysize (g:second d)))) (g:emit '(PSH))
						(g:emit '(MCPY)) (g:emit '(ADJ 3)))
				(begin
					(g:emit (list 'LEA (- g:loc val)))
					(g:emit '(PSH))
					(g:expr (g:second init))
					(if (g:isptr (g:second d))
						(if (g:dropped? (g:nz (index d 5)) (g:qualof (g:second init)))
							(g:qwarn "initialization") nil)
					nil)
					(g:emit '(SI))))
			(g:localaggr d val)))
		(if (if (= (head init) 'braces)
				(not (if (g:ll? (- (g:second d) g:PTR)) true (g:rowty? (- (g:second d) g:PTR)))) false)
			(begin
				(if (> (length (g:second init)) size)
					(g:die (+ "too many initializers for " (g:third d))) nil)
				(g:linit/elems val (g:second init) size 0 bytewise))
		(if (if (= (head init) 'str) (= bytewise true) false)
			(begin				(if (< size (length (g:second init)))
					(g:die (+ "string does not fit the array " (g:third d))) nil)
				(g:linit/elems val (g:strbytes (g:second init) 0 (list))
					size 0 true))
		(if (if (= (head init) 'agg) true (= (head init) 'braces)) (g:localaggr d val)		(g:die (+ "unsupported array initializer for " (g:third d))))))))))))

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
			(list (g:third d) 'loc (g:second d) g:lcur (g:lbytes d) (g:nz (index d 5)))
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
		(g:texpr (g:third s))
		(g:emit (list 'BNZ (list 'code lb)))
		(g:label! lend)
		(set! g:brk ob)
		(set! g:cont oc))))

(define g:function (lambda (d)
	(begin
		(define n (g:third d))
		(set! g:curfn n)
		(set! g:curret (g:second d))
		(set! g:curretq (g:nz (index d 8)))
		(define ps (index d 3))
		(define attrs (index d 5))
		(define ls (tail (index d 6)))
		(define body (index d 7))
		(define s (if (g:helperdef? d) (g:lookup n g:llsyms) (g:lookup n g:syms)))
		(define oldsyms g:syms)
		;; a variadic's ... occupies one unnamed slot (c4cc's fake
		;; parameter), counted in argc so va_start's &LAST - 1 lands on it
		(define argc (+ (+ (g:params ps 0) (if (index d 4) 1 0)) (if (g:sret? (g:second d)) 1 0)))
		(set! g:loc (+ argc 1))
		(define fini (g:localdefs ls (+ argc 1)))
		;; block-scoped declarations deeper in the body still need
		;; their frame words reserved by ENT; a pre-scan counts them
		(define extra (g:scanstmt body))
		(set! g:lcur fini)
		;; -g: the function's entry in the map's function table and its
		;; first row, at the line of its name; both size 0, ahead of the
		;; label so they start where the function does
		(define fl (if p:lines (p:assoc n p:fnlocs) false))
		(if fl
			(begin
				(g:emit (list 'func n (g:second fl)))
				(g:emit (list 'line (g:second fl))))
			nil)
		(g:label! (index s 3))
		;; the long long temporaries go below everything else, and how
		;; many there are is known at the end: (frame ID WORDS) is
		;; resolved then (g:resolvecode)
		(set! g:tbase (+ fini extra))
		(set! g:tcur 0)
		(set! g:tmax 0)
		(define fid g:fid)
		(set! g:fid (+ g:fid 1))
		(g:emit (list 'ENT (list 'frame fid (+ (- fini g:loc) extra))))
		(g:localinits ls)
		(set! g:ulabs (list))
		(set! g:uplaced (list))
		(set! g:uwanted (list))
		(g:stmts (tail body))
		(g:ucheck g:uwanted)
		(set! g:frames (g:cons (list fid g:tmax) g:frames))
		;; the exit falling off the end is the closing brace's (-g)
		(if (= (head (g:lastreal g:code)) 'LEV) nil
			(begin
				(if fl (g:emit (list 'line (g:third fl))) nil)
				(g:emit '(LEV))))
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
		(if (= h 'structdef) nil       ;; g:datapass registered layouts
		(if (= h 'global) nil          ;; g:datapass allocated storage
		(if (= h 'func)
			;; a long long helper only if something called it
			(if (g:helperdef? d)
				(if (g:lookup (g:third d) g:llsyms) (g:function d) nil)
			(g:function d))
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
;; them undefined for c4rlink, value 0. Entries carry their class:
;; 129 (Fun) from prototypes, 131 (Glo) from extern data
(define g:extsection (lambda (exts id acc)
	(if (empty? exts) (g:reverse acc)
	(begin
		(define x (head exts))
		(next g:extsection (tail exts) (+ id 1)
			(g:cons (list id (g:second x) (index x 3)
				(bit:or 16 (bit:or (if (g:third x) 32 0)
					(if (= (index x 4) nil) 0 64)))
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

;; ---- -fpic (gen:pic) ----
;;
;; Position-independent data, for a shared library whose code C4IX loads
;; once and shares between tasks while each task keeps its own copy of
;; the data (src/c4ix/loader.c). A reference to data -- a global, a
;; string, a jump table, an extern global -- is normally an IMM of its
;; absolute address. Here it becomes its offset in the data plus the data
;; base, which the code reads from __c4_dbase: a word the loader points
;; the code at, and the scheduler keeps holding the running task's data.
;;   IMM off ; PSH ; IMM __c4_dbase ; LI ; ADD
;; The IMM keeps its data relocation; the loader, seeing __c4_dbase,
;; writes such relocations as offsets. Four instructions more per data
;; reference, in exchange for one copy of the code however many tasks
;; use it. Object mode only (-c), since a library is linked from objects.
(define gen:pic false)

;; the class (129 Fun, 131 Glo) of extern K, in id order
(define g:extclass (lambda (k l)
	(if (empty? l) 0
	(if (= k 0) (index (head l) 3)
	(next g:extclass (- k 1) (tail l))))))

;; is I an IMM of a data address -- (data N), or an extern global?
(define g:picref? (lambda (i exts)
	(if (= 2 (length i))
		(if (= (head i) 'IMM)
			(if (= 'list (typeof (g:second i)))
				(if (= 'data (head (g:second i))) true
				(if (= 'extph (head (g:second i)))
					(= 131 (g:extclass (g:second (g:second i)) exts))
					false))
				false)
			false)
		false)))

(define g:picify (lambda (code db exts acc)
	(if (empty? code) (g:reverse acc)
	(begin
		(define i (head code))
		(if (g:picref? i exts)
			(next g:picify (tail code) db exts
				(g:cons '(ADD) (g:cons '(LI) (g:cons (list 'IMM (list 'extph db))
					(g:cons '(PSH) (g:cons i acc))))))
			(next g:picify (tail code) db exts (g:cons i acc)))))))

;; ---- module assembly ----

;; BSS finalize: bss-relative offsets become absolute data offsets once
;; the base (the word-aligned end of the initialized data) is known.
;; gen:bssextra carries the extra in-memory bytes beyond the data image
;; (alignment pad + bss size) so c4r:encode can record MEMSZ.
(define gen:bssextra 0)

;; rebuild a symbol list, resolving 'glo entries whose offset is a bss
;; (negative-encoded) offset to base + relative.
;;
;; This and g:resolvecode below walk the whole module, so they loop
;; (next, into an accumulator) rather than recurse: a recursion here is
;; one stack frame per entry of the module's code. Compiled by c4sc that
;; is 56 bytes a frame, so C4KE's 256 KB task stack holds about 4,700
;; entries; src/c4ix/sched.c passed that at cd59860 (6,011) and is at
;; 8,022 now, and the climb (`b4ke -f c4ix.b4k`) ran through the stack's
;; guard and into the shell's code below it.
(define g:resolvesyms (lambda (ss base acc)
	(if (empty? ss) (g:reverse acc)
	(begin
		(define s (head ss))
		(next g:resolvesyms (tail ss) base (g:cons
			(if (if (= (g:second s) 'glo) (g:bss? (index s 3)) false)
				(list (head s) 'glo (g:third s)
					(+ base (g:bssoff (index s 3))) (index s 4) (index s 5) (index s 6))
				s)
			acc))))))

;; rewrite (bss N) operands in the code to absolute (data base+N), and
;; (frame ID WORDS) to the frame's final size
(define g:resolvecode (lambda (cs base acc)
	(if (empty? cs) (g:reverse acc)
	(begin
		(define i (head cs))
		(next g:resolvecode (tail cs) base (g:cons
			(if (if (= 2 (length i))
					(if (= 'list (typeof (second i))) (= 'bss (head (second i))) false)
					false)
				(list (head i) (list 'data (+ base (second (second i)))))
			(if (if (= 2 (length i))
					(if (= 'list (typeof (second i))) (= 'frame (head (second i))) false)
					false)
				(list (head i) (+ (index (second i) 2) (g:framewords (second (second i)) g:frames)))
				i))
			acc))))))

(define gen:module (lambda (ast)
	(begin
		(set! g:code (list))
		(set! g:nlabel 0)
		(set! g:ilabs (list))
		(set! g:data (string:alloc g:DMAX))
		(set! g:dlen 0)
		(set! g:blen 0)
		(set! gen:bssextra 0)
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
		(set! g:helps (list))
		(set! g:llsrc (list))
		(set! g:llsyms (list))
		(set! g:fpon false)
		(set! g:rows (list))
		(set! g:frames (list))
		(set! g:fid 0)
		(set! g:tcur 0)
		(set! g:tmax 0)
		(g:sysinit g:syscalls)
		(g:structpass (tail ast))
		(g:prepass (tail ast))
		(g:protopass (tail ast))
		(g:datapass (tail ast) (list))
		(g:decls (tail ast))
		(g:emitstubs (g:reverse g:stubs))
		(g:emithelpers g:HELPERS)
		(define m (g:lookup "main" g:syms))
		(if m nil
			(if gen:objmode nil (g:die "no main function")))
		;; BSS finalize: bss base = word-aligned end of the data image;
		;; resolve bss-relative offsets in both the symbol table and the
		;; code before either is serialized. gen:bssextra = the bytes the
		;; loader must zero-fill beyond the stored data (align pad + bss).
		(define bssbase (* g:WORD (/ (+ g:dlen g:WMASK) g:WORD)))
		(set! gen:bssextra (- (+ bssbase g:blen) g:dlen))
		(define syms (g:symsection (g:resolvesyms (g:reverse g:syms) bssbase (list)) 0 (list)))
		(define code (g:resolvecode (g:reverse g:code) bssbase (list)))
		(set! g:dpatches (g:resolvedglo g:dpatches bssbase (list)))
		;; -fpic: every data reference relative to __c4_dbase, a new
		;; extern global the loader supplies
		(if (if gen:pic gen:objmode false)
			(begin
				(define db g:nexterns)
				(set! g:externs (g:cons (list "__c4_dbase" g:INT nil 131 nil) g:externs))
				(set! g:nexterns (+ g:nexterns 1))
				(set! code (g:picify code db (g:reverse g:externs) (list))))
			nil)
		(if (> g:nexterns 0)
			(begin
				(define base (length syms))
				(set! syms (+ syms (g:extsection (g:reverse g:externs) base (list))))
				(set! code (g:remapext code base (list)))
				(set! g:dpatches (g:remapdsym g:dpatches base (list))))
			nil)
		(list 2 (* 8 g:WORD) (if m (list 'code (index m 3)) -1)
			code
			(string:substr g:data 0 g:dlen)
			syms
			(g:reverse g:cons*)
			(g:reverse g:des*)
			(g:reverse g:dpatches)))))

)
