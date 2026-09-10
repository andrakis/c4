;; c4lc-pp.lisp -- the preprocessor (L9)
;;
;; Removes the last host-toolchain dependency from the pipeline. Until
;; now every source went through `gcc -E` before c4lc saw it, which
;; meant c4lc could not compile anything inside the OS it targets --
;; C4KE has no gcc. This module does the job in c4sp instead.
;;
;; It works on TOKENS, not text, which is the only way macro
;; expansion is correct: the lexer (in lex:pp mode) hands back '#' as
;; a Hash token, splices backslash-newlines, and returns a <header>
;; name as one Str token. A directive runs from a Hash to the end of
;; its LINE, which is why the splicing matters.
;;
;; Supported: #include (both "..." and <...>, searched along -I),
;; #define (object-like and function-like), #undef, #ifdef, #ifndef,
;; #if / #elif / #else / #endif with constant expressions, and the
;; `# 123 "file"` line markers gcc -E emits (skipped, so this can
;; consume already-preprocessed sources too).
;;
;; Not supported, and rejected rather than mis-expanded: stringize
;; (#param) and paste (##). Nothing in this tree uses them. Variadic
;; macros are likewise absent.

(begin

(define pp:macros nil)      ;; (NAME PARAMS BODY), PARAMS nil = object-like
(define pp:paths nil)       ;; -I directories, searched in order
(define pp:depth 0)         ;; include nesting, guards runaway recursion
(define pp:MAXDEPTH 24)
(define pp:MAXEXPAND 64)    ;; rescan limit per token position

(define pp:die (lambda (m) (error (+ "c4lc: preprocessor: " m))))

;; ---- small list helpers (same style as the rest of c4lc) ----

(define pp:cons (lambda (x l) (+ (list x) l)))
;; Is any level of the conditional stack skipping? Used by #else and
;; #elif to ask about their ENCLOSING levels, which is the difference
;; between "this branch was not taken" and "none of this is being read
;; at all".
(define pp:any-skip? (lambda (l)
	(if (empty? l) false (if (head l) true (next pp:any-skip? (tail l))))))
(define pp:rev (lambda (l acc)
	(if (empty? l) acc (next pp:rev (tail l) (pp:cons (head l) acc)))))
(define pp:reverse (lambda (l) (pp:rev l nil)))
(define pp:append (lambda (a b)
	(if (empty? a) b (pp:append/2 (pp:reverse a) b))))
(define pp:append/2 (lambda (ra b)
	(if (empty? ra) b (next pp:append/2 (tail ra) (pp:cons (head ra) b)))))

(define pp:kind (lambda (t) (head t)))
(define pp:val (lambda (t) (head (tail t))))
(define pp:line (lambda (t) (head (tail (tail t)))))

;; ---- the macro table ----

(define pp:find (lambda (n l)
	(if (empty? l) false
	(if (= (head (head l)) n) (head l)
	(next pp:find n (tail l))))))

(define pp:defined? (lambda (n) (if (pp:find n pp:macros) true false)))

(define pp:define! (lambda (n params body)
	(begin
		(pp:undef! n)
		(set! pp:macros (pp:cons (list n params body) pp:macros)))))

(define pp:undef! (lambda (n)
	(set! pp:macros (pp:undef/2 n pp:macros nil))))
(define pp:undef/2 (lambda (n l acc)
	(if (empty? l) (pp:reverse acc)
	(if (= (head (head l)) n) (next pp:undef/2 n (tail l) acc)
	(next pp:undef/2 n (tail l) (pp:cons (head l) acc))))))

;; ---- splitting a directive off the front of the stream ----
;;
;; A directive is every token sharing the Hash's line. Returns
;; (DIRECTIVE-TOKENS REST).
;;
;; With Stop true it ALSO stops at the next Hash, and that is not a
;; nicety. #include splices the included file's tokens in front of the
;; rest, and those carry THEIR file's line numbers, so after a splice the
;; numbers in the stream stop increasing -- they restart. A header whose
;; last line is a directive (`#endif`: every guarded header there is),
;; included from line N of a file whose line N is itself a directive,
;; shares a line number that means nothing. Without the stop the `#endif`
;; swallowed the next `#include` as part of its own line and THE HEADER
;; WAS SILENTLY NEVER INCLUDED -- no error, just missing declarations and
;; an "undefined identifier" from codegen a long way away.
;;
;; Stop is false for #define alone, whose body is the one place a '#' can
;; legitimately appear (stringize, `#define STR(x) #x`, and `##` paste).
;; So a header whose last line is a #define can still swallow what
;; follows it; a header whose last line is #endif -- which is all of
;; them -- cannot. Pinned by c4lc_ppinc.c in test-c4lc.
(define pp:takeline (lambda (toks ln acc stop)
	(if (empty? toks) (list (pp:reverse acc) toks)
	(if (= (pp:kind (head toks)) 'Eof) (list (pp:reverse acc) toks)
	(if (if stop (= (pp:kind (head toks)) 'Hash) false) (list (pp:reverse acc) toks)
	(if (= (pp:line (head toks)) ln)
		(next pp:takeline (tail toks) ln (pp:cons (head toks) acc) stop)
	(list (pp:reverse acc) toks)))))))

;; ---- #define ----
;;
;; Function-like only when the '(' touches the name -- "#define A (x)"
;; defines A as the token sequence "(x)", which is why the lexer's
;; line numbers alone are not enough and c4cc gets this wrong. We
;; approximate the standard rule by requiring the paren to be the very
;; next token, which is true whenever there is no space.
(define pp:dodefine (lambda (d)
	(begin
		(if (empty? d) (pp:die "#define needs a name") nil)
		(define nt (head d))
		(if (= (pp:kind nt) 'Id) nil (pp:die "#define needs a name"))
		(define n (pp:val nt))
		(define rest (tail d))
		;; function-like only when the paren touched the name (the
		;; lexer's fourth field), never merely followed it
		(if (if (= (length nt) 4) (= (index nt 3) 1) false)
			(begin
				(define pr (pp:params (tail rest) nil))
				(pp:define! n (head pr) (head (tail pr))))
			(pp:define! n false rest)))))

;; parameter list after '('; returns (PARAMS BODY)
(define pp:params (lambda (toks acc)
	(if (empty? toks) (pp:die "unterminated macro parameter list")
	(if (= (pp:kind (head toks)) 'Rparen)
		(list (pp:reverse acc) (tail toks))
	(if (= (pp:kind (head toks)) 'Comma)
		(next pp:params (tail toks) acc)
	(if (= (pp:kind (head toks)) 'Id)
		(next pp:params (tail toks) (pp:cons (pp:val (head toks)) acc))
	(pp:die "bad macro parameter")))))))

;; ---- #include ----

(define pp:trypaths (lambda (name paths)
	(if (empty? paths) false
	(begin
		(define full (+ (+ (head paths) "/") name))
		(if (file:exists (file:path full)) full
		(next pp:trypaths name (tail paths)))))))

(define pp:resolve (lambda (name)
	(if (file:exists (file:path name)) name
	(begin
		(define p (pp:trypaths name pp:paths))
		(if p p (pp:die (+ "cannot find include: " name)))))))

;; Lex a file and splice its tokens (minus the trailing Eof) in front
;; of what is left, so the result is preprocessed by the same walk.
(define pp:include (lambda (name rest)
	(begin
		(if (> pp:depth pp:MAXDEPTH) (pp:die "#include nested too deeply") nil)
		(define path (pp:resolve name))
		(set! pp:depth (+ pp:depth 1))
		(define toks (lex:file path))
		(set! pp:depth (- pp:depth 1))
		(pp:append (pp:dropeof toks nil) rest))))

(define pp:dropeof (lambda (toks acc)
	(if (empty? toks) (pp:reverse acc)
	(if (= (pp:kind (head toks)) 'Eof) (pp:reverse acc)
	(next pp:dropeof (tail toks) (pp:cons (head toks) acc))))))

;; ---- constant expressions for #if ----
;;
;; Enough of the grammar to serve real headers: defined(X) and
;; defined X, integer literals, macros (expanded first), the unary
;; operators ! and -, and the binary set below with C's precedence.
;; An identifier that is not a macro is 0, as the standard says.

(define pp:etoks nil)

(define pp:epeek (lambda ()
	(if (empty? pp:etoks) 'Eof (pp:kind (head pp:etoks)))))
(define pp:enext (lambda ()
	(begin
		(define t (head pp:etoks))
		(set! pp:etoks (tail pp:etoks))
		t)))

(define pp:eprimary (lambda ()
	(begin
		(define k (pp:epeek))
		(if (= k 'Num) (pp:val (pp:enext))
		(if (= k 'Lparen)
			(begin
				(pp:enext)
				(define v (pp:eor))
				(if (= (pp:epeek) 'Rparen) (pp:enext) nil)
				v)
		(if (= k 'Not) (begin (pp:enext) (if (= 0 (pp:eprimary)) 1 0))
		(if (= k 'Sub) (begin (pp:enext) (- 0 (pp:eprimary)))
		(if (= k 'Id)
			(begin
				(define t (pp:enext))
				(if (= (pp:val t) "defined") (pp:edefined)
				;; any surviving identifier is an undefined macro: 0
				0))
		0))))))))

;; defined(X) or defined X
(define pp:edefined (lambda ()
	(begin
		(define paren (= (pp:epeek) 'Lparen))
		(if paren (pp:enext) nil)
		(define t (pp:enext))
		(if paren (if (= (pp:epeek) 'Rparen) (pp:enext) nil) nil)
		(if (pp:defined? (pp:val t)) 1 0))))

(define pp:emul (lambda ()
	(begin
		(define l (pp:eprimary))
		(next pp:emul/2 l))))
(define pp:emul/2 (lambda (l)
	(begin
		(define k (pp:epeek))
		(if (= k 'Mul) (begin (pp:enext) (next pp:emul/2 (* l (pp:eprimary))))
		(if (= k 'Div) (begin (pp:enext) (next pp:emul/2 (/ l (pp:eprimary))))
		l)))))

(define pp:eadd (lambda ()
	(begin
		(define l (pp:emul))
		(next pp:eadd/2 l))))
(define pp:eadd/2 (lambda (l)
	(begin
		(define k (pp:epeek))
		(if (= k 'Add) (begin (pp:enext) (next pp:eadd/2 (+ l (pp:emul))))
		(if (= k 'Sub) (begin (pp:enext) (next pp:eadd/2 (- l (pp:emul))))
		l)))))

(define pp:ecmp (lambda ()
	(begin
		(define l (pp:eadd))
		(next pp:ecmp/2 l))))
(define pp:ecmp/2 (lambda (l)
	(begin
		(define k (pp:epeek))
		(if (= k 'Lt) (begin (pp:enext) (next pp:ecmp/2 (if (< l (pp:eadd)) 1 0)))
		(if (= k 'Gt) (begin (pp:enext) (next pp:ecmp/2 (if (> l (pp:eadd)) 1 0)))
		(if (= k 'Le) (begin (pp:enext) (next pp:ecmp/2 (if (<= l (pp:eadd)) 1 0)))
		(if (= k 'Ge) (begin (pp:enext) (next pp:ecmp/2 (if (>= l (pp:eadd)) 1 0)))
		(if (= k 'Eq) (begin (pp:enext) (next pp:ecmp/2 (if (= l (pp:eadd)) 1 0)))
		(if (= k 'Ne) (begin (pp:enext) (next pp:ecmp/2 (if (= l (pp:eadd)) 0 1)))
		l)))))))))

(define pp:eand (lambda ()
	(begin
		(define l (pp:ecmp))
		(next pp:eand/2 l))))
(define pp:eand/2 (lambda (l)
	(if (= (pp:epeek) 'Land)
		(begin
			(pp:enext)
			(define r (pp:ecmp))
			(next pp:eand/2 (if (= 0 l) 0 (if (= 0 r) 0 1))))
	l)))

(define pp:eor (lambda ()
	(begin
		(define l (pp:eand))
		(next pp:eor/2 l))))
(define pp:eor/2 (lambda (l)
	(if (= (pp:epeek) 'Lor)
		(begin
			(pp:enext)
			(define r (pp:eand))
			(next pp:eor/2 (if (= 0 l) (if (= 0 r) 0 1) 1)))
	l)))

;; Evaluate a directive's tokens: expand macros first (so `#if FOO`
;; sees FOO's body), then parse. Returns true/false.
(define pp:evalif (lambda (toks)
	(begin
		(set! pp:etoks (pp:expandall toks nil))
		(define v (pp:eor))
		(if (= v 0) false true))))

;; ---- macro expansion ----

;; Expand every macro in a token list (used for #if lines, where
;; there is no surrounding stream to push back onto).
(define pp:expandall (lambda (toks acc)
	(if (empty? toks) (pp:reverse acc)
	(begin
		(define t (head toks))
		(if (= (pp:kind t) 'Id)
			(begin
				(define m (pp:find (pp:val t) pp:macros))
				;; `defined X` must not expand X
				(if (if m (= (pp:val t) "defined") false) (set! m false) nil)
				(if (if (empty? acc) false (= (pp:val (head acc)) "defined"))
					(set! m false) nil)
				(if m
					(begin
						(define r (pp:apply m (tail toks) (pp:line t)))
						(next pp:expandall (pp:append (head r) (head (tail r))) acc))
				(next pp:expandall (tail toks) (pp:cons t acc))))
		(next pp:expandall (tail toks) (pp:cons t acc)))))))

;; Apply macro M at the head of REST. Returns (EXPANSION REMAINDER).
(define pp:apply (lambda (m rest ln)
	(begin
		(define params (head (tail m)))
		(define body (head (tail (tail m))))
		;; false = object-like. An empty LIST is a function-like macro
		;; with no parameters -- "#define f() 0" is legal C, and
		;; treating it as object-like expanded f() to the body with a
		;; stray "()" left behind after it.
		(if (= params false)
			(list (pp:reline body ln) rest)
		(begin
			;; function-like: only expands when an argument list
			;; follows, otherwise the name stands for itself
			(if (if (empty? rest) true (= (pp:kind (head rest)) 'Lparen)) nil
				(pp:die (+ "macro needs arguments: " (head m))))
			(if (empty? rest)
				(list (list (list 'Id (head m) ln)) rest)
			(begin
				(define ar (pp:args (tail rest) nil nil 0))
				(define args (head ar))
				(define after (head (tail ar)))
				;; Arguments are macro-expanded BEFORE substitution --
				;; except as operands of # or ##, which see them raw.
				;; That distinction is the whole reason XSTR(V) gives
				;; the value of V while STR(V) gives "V".
				(define eargs (pp:expandargs args nil))
				(list (pp:reline (pp:subst body params args eargs nil) ln)
					after))))))))

(define pp:expandargs (lambda (args acc)
	(if (empty? args) (pp:reverse acc)
	(next pp:expandargs (tail args)
		(pp:cons (pp:expandall (head args) nil) acc)))))

;; Collect comma-separated arguments up to the matching ')'.
;; Returns (ARGS REST); each arg is a token list.
(define pp:args (lambda (toks cur acc d)
	(if (empty? toks) (pp:die "unterminated macro arguments")
	(begin
		(define t (head toks))
		(define k (pp:kind t))
		(if (if (= k 'Rparen) (= d 0) false)
			(list (pp:reverse (pp:cons (pp:reverse cur) acc)) (tail toks))
		(if (if (= k 'Comma) (= d 0) false)
			(next pp:args (tail toks) nil (pp:cons (pp:reverse cur) acc) d)
		(next pp:args (tail toks) (pp:cons t cur) acc
			(if (= k 'Lparen) (+ d 1) (if (= k 'Rparen) (- d 1) d)))))))))

;; ---- spelling tokens back into text ----
;;
;; Needed by stringize and paste, and by nothing else. Identifiers
;; and numbers are what those operators are actually used on; the
;; punctuation table covers the rest of what can plausibly appear
;; inside a macro argument.
(define pp:optext '(
	(Lparen "(") (Rparen ")") (Lbrace "{") (Rbrace "}")
	(Lbrak "[") (Rbrak "]") (Semi ";") (Comma ",") (Dot ".")
	(Arrow "->") (Assign "=") (Eq "==") (Ne "!=") (Lt "<") (Gt ">")
	(Le "<=") (Ge ">=") (Add "+") (Sub "-") (Mul "*") (Div "/")
	(Mod "%") (And "&") (Or "|") (Xor "^") (Not "!") (Tilde "~")
	(Land "&&") (Lor "||") (Shl "<<") (Shr ">>") (Inc "++")
	(Dec "--") (Cond "?") (Colon ":")
	(Int "int") (Char "char") (Void "void") (If "if") (Else "else")
	(While "while") (For "for") (Return "return") (Sizeof "sizeof")
	(Struct "struct") (Union "union") (Enum "enum") (Static "static")
	(Extern "extern") (Typedef "typedef") (Break "break")
	(Continue "continue") (Switch "switch") (Case "case")
	(Default "default") (Do "do")))

(define pp:oplook (lambda (k l)
	(if (empty? l) false
	(if (= (head (head l)) k) (head (tail (head l)))
	(next pp:oplook k (tail l))))))

(define pp:spell (lambda (t)
	(begin
		(define k (pp:kind t))
		(if (= k 'Id) (pp:val t)
		(if (= k 'Num) (+ "" (pp:val t))
		(if (= k 'Str) (pp:val t)
		(begin
			(define o (pp:oplook k pp:optext))
			(if o o "")))))))) 

;; Spell a whole token list, space-separated where two words would
;; otherwise run together. cpp's exact whitespace rules are subtler;
;; this matches on the shapes that occur in practice.
(define pp:spelllist (lambda (toks acc first)
	(if (empty? toks) acc
	(begin
		(define txt (pp:spell (head toks)))
		(next pp:spelllist (tail toks)
			(if first txt (+ (+ acc " ") txt))
			false)))))

;; token ## token: splice the spellings and lex the result, which is
;; how a paste can produce an identifier, a number, or an operator
;; without the preprocessor having to know which.
(define pp:paste (lambda (a b)
	(begin
		(define txt (+ (pp:spell a) (pp:spell b)))
		(pp:dropeof (lex:string txt) nil))))

;; Replace parameters in BODY with the matching argument token lists.
(define pp:subst (lambda (body params args eargs acc)
	(if (empty? body) (pp:reverse acc)
	(begin
		(define t (head body))
		(define k (pp:kind t))
		;; stringize: '#' before a parameter becomes a string literal
		;; holding that argument's text
		(if (if (= k 'Hash) (if (empty? (tail body)) false true) false)
			(begin
				(define nx (head (tail body)))
				(define sa (if (= (pp:kind nx) 'Id)
					(pp:argfor (pp:val nx) params args) false))
				(if sa
					(next pp:subst (tail (tail body)) params args eargs
						(pp:cons (list 'Str (pp:spelllist sa "" true)
							(pp:line t)) acc))
				(next pp:subst (tail body) params args eargs (pp:cons t acc))))
		;; paste: A ## B, with either side possibly a parameter
		(if (if (empty? (tail body)) false
				(= (pp:kind (head (tail body))) 'HashHash))
			(begin
				(define rhs (tail (tail body)))
				(if (empty? rhs)
					(next pp:subst (tail body) params args eargs (pp:cons t acc))
				(begin
					(define lt (pp:lastof (pp:one t params args) t))
					(define rt (pp:firstof (pp:one (head rhs) params args)
						(head rhs)))
					(next pp:subst (tail rhs) params args eargs
						(pp:revonto (pp:paste lt rt) acc)))))
		(begin
			;; ordinary parameter: the EXPANDED argument
			(define a (if (= k 'Id) (pp:argfor (pp:val t) params eargs) false))
			(if a (next pp:subst (tail body) params args eargs (pp:revonto a acc))
			(next pp:subst (tail body) params args eargs (pp:cons t acc)))))))))) 

;; the argument list for a parameter token, or false
(define pp:one (lambda (t params args)
	(if (= (pp:kind t) 'Id) (pp:argfor (pp:val t) params args) false)))
(define pp:lastof (lambda (l dflt)
	(if l (if (empty? l) dflt (pp:lastof/2 l)) dflt)))
(define pp:lastof/2 (lambda (l)
	(if (empty? (tail l)) (head l) (next pp:lastof/2 (tail l)))))
(define pp:firstof (lambda (l dflt)
	(if l (if (empty? l) dflt (head l)) dflt)))

(define pp:argfor (lambda (n params args)
	(if (empty? params) false
	(if (= (head params) n)
		(if (empty? args) (list) (head args))
	(next pp:argfor n (tail params)
		(if (empty? args) args (tail args)))))))

(define pp:revonto (lambda (l acc)
	(if (empty? l) acc (next pp:revonto (tail l) (pp:cons (head l) acc)))))

;; Expanded tokens carry the invocation's line so later directive
;; splitting (and any diagnostics) stay sane.
(define pp:reline (lambda (toks ln)
	(pp:reline/2 toks ln nil)))
(define pp:reline/2 (lambda (toks ln acc)
	(if (empty? toks) (pp:reverse acc)
	(begin
		(define t (head toks))
		(next pp:reline/2 (tail toks) ln
			(pp:cons (list (pp:kind t) (pp:val t) ln) acc))))))

;; ---- the main walk ----
;;
;; SKIP is the conditional stack: a list whose head is true when the
;; branch being read should be dropped. Directives are still parsed
;; while skipping, so nesting stays correct.

(define pp:directive (lambda (d rest skip)
	(begin
		(if (empty? d) (list rest skip)     ;; a bare '#' is a null directive
		(begin
			(define t (head d))
			;; "if" and "else" are C keywords, so the lexer hands them
			;; back as keyword tokens rather than identifiers -- map
			;; them home or #if/#else would silently do nothing.
			(define name (if (= (pp:kind t) 'Id) (pp:val t)
				(if (= (pp:kind t) 'If) "if"
				(if (= (pp:kind t) 'Else) "else" ""))))
			(define body (tail d))
			(define skipping (if (empty? skip) false (head skip)))
			;; conditionals are handled even inside a skipped branch
			(if (= name "ifdef")
				(list rest (pp:cons (if skipping true
					(if (pp:defined? (pp:val (head body))) false true)) skip))
			(if (= name "ifndef")
				(list rest (pp:cons (if skipping true
					(if (pp:defined? (pp:val (head body))) true false)) skip))
			(if (= name "if")
				(list rest (pp:cons (if skipping true
					(if (pp:evalif body) false true)) skip))
			(if (= name "elif")
				(begin
					(if (empty? skip) (pp:die "#elif without #if") nil)
					;; only reconsider if this level was skipping AND
					;; no earlier branch has been taken -- and NEVER if an
					;; enclosing level is skipping, or this would start
					;; reading a branch nested inside dropped text.
					(list rest (cons
						(if (pp:any-skip? (tail skip)) true
						(if (head skip) (if (pp:evalif body) false true) true))
						(tail skip))))
			(if (= name "else")
				(begin
					(if (empty? skip) (pp:die "#else without #if") nil)
					;; #else INHERITS. It used to simply flip the top of
					;; the stack, which is right at the outermost level and
					;; wrong everywhere else: an #else nested inside a
					;; branch that is being dropped flipped "skipping" off
					;; and started reading text no build should ever see.
					;;
					;; #ifdef already got this right (it consults
					;; `skipping` above); #else and #elif did not. What it
					;; cost: raycast.c has
					;;     #ifdef RC_DOS ... #else #ifdef RC_C4 ... #else
					;;     int plat_emit (char *b) ...
					;; and the DOS arm #defines plat_emit(b). Built with
					;; -D RC_DOS=1 the inner #else woke up, the skipped
					;; declaration was read WITH the macro expanded over
					;; its own name, and c4lc reported "bad parameter
					;; declaration" on a line that build never uses.
					;; raycast-dos32.c4r could not be built at all, which
					;; took c4dos-c4ix32 with it, and with that the climb
					;; disk the browser boots.
					(list rest (pp:cons
						(if (pp:any-skip? (tail skip)) true
						(if (head skip) false true)) (tail skip))))
			(if (= name "endif")
				(begin
					(if (empty? skip) (pp:die "#endif without #if") nil)
					(list rest (tail skip)))
			(if skipping (list rest skip)
			(if (= name "define") (begin (pp:dodefine body) (list rest skip))
			(if (= name "undef") (begin (pp:undef! (pp:val (head body))) (list rest skip))
			(if (= name "include")
				(begin
					(define ex (pp:expandall body nil))
					(if (empty? ex) (pp:die "#include needs a file") nil)
					(if (= (pp:kind (head ex)) 'Str) nil
						(pp:die "#include needs \"file\" or <file>"))
					(list (pp:include (pp:val (head ex)) rest) skip))
			(if (= name "") (list rest skip)          ;; gcc's `# 12 "f"` markers
			(if (= name "pragma") (list rest skip)
			(if (= name "error") (pp:die "#error")
			(if (= name "line") (list rest skip)
			(pp:die (+ "unknown directive: #" name)))))))))))))))))))))

(define pp:go (lambda (toks skip acc)
	(if (empty? toks) (pp:reverse acc)
	(begin
		(define t (head toks))
		(if (= (pp:kind t) 'Eof)
			(begin
				(if (empty? skip) nil (pp:die "unterminated #if"))
				(pp:reverse (pp:cons t acc)))
		(if (= (pp:kind t) 'Hash)
			(begin
				;; #define is the only directive whose body may contain a '#'
				(define dnm (if (empty? (tail toks)) "" (+ "" (pp:val (head (tail toks))))))
				(define dl (pp:takeline (tail toks) (pp:line t) nil (if (= dnm "define") false true)))
				(define r (pp:directive (head dl) (head (tail dl)) skip))
				(next pp:go (head r) (head (tail r)) acc))
		(if (if (empty? skip) false (head skip))
			(next pp:go (tail toks) skip acc)     ;; inside a dead branch
		(if (= (pp:kind t) 'Id)
			(begin
				(define m (pp:find (pp:val t) pp:macros))
				(if m
					(begin
						(define r (pp:apply m (tail toks) (pp:line t)))
						;; push the expansion back so it is rescanned
						(next pp:go (pp:append (head r) (head (tail r))) skip acc))
				(next pp:go (tail toks) skip (pp:cons t acc))))
		(next pp:go (tail toks) skip (pp:cons t acc))))))))))

;; ---- entry points ----

(define pp:reset (lambda ()
	(begin
		(set! pp:macros nil)
		(set! pp:depth 0))))

;; -D NAME or -D NAME=VALUE
(define pp:predefine (lambda (spec)
	(begin
		(define eq (pp:eqpos spec 0))
		(if (< eq 0)
			(pp:define! spec false (list (list 'Num 1 0)))
		(begin
			(define n (string:substr spec 0 eq))
			(define v (string:substr spec (+ eq 1) (- (length spec) (+ eq 1))))
			(pp:define! n false (lex:string v)))))))
(define pp:predefines (lambda (l)
	(if (empty? l) nil
	(begin
		(pp:predefine (head l))
		(next pp:predefines (tail l))))))

(define pp:eqpos (lambda (s i)
	(if (>= i (length s)) -1
	(if (= (string:byte s i) 61) i
	(next pp:eqpos s (+ i 1))))))

;; Preprocess a file: returns a token list ready for parse:program.
(define pp:file (lambda (path)
	(begin
		(set! lex:pp true)
		(define toks (lex:file path))
		(pp:go toks nil nil))))

)
