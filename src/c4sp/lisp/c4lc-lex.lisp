;; c4lc-lex.lisp -- C tokenizer for c4lc (docs/c4lc-design.md, L0)
;;
;; (lex:file PATH) / (lex:string SRC) -> list of (KIND VALUE LINE)
;; tokens ending with (Eof 0 LINE). KIND is an atom (design 5); VALUE is
;; 0 except Num (integer), Id (name string), Str (decoded bytes).
;;
;; Mirrors c4cc's next() byte for byte, quirky escape table included
;; (\t -> 8, \r -> 10). Deliberate divergences are listed in design
;; 5.1: block comments count their newlines, '' is Num 0.
;;
;; Falsiness is Lisp-style (false, nil/() and integer 0); predicates
;; here still return real booleans. Token VALUES can be a falsy 0 --
;; always branch on the token KIND, never the value.

(begin

;; ---- small list helpers (same idiom as c4r.lisp) ----

(define lex:reverse (lambda (l) (next lex:reverse/2 l (list))))
(define lex:reverse/2 (lambda (l acc)
	(if (empty? l) acc (next lex:reverse/2 (tail l) (+ (list (head l)) acc)))))

;; (lex:cons x l) shares l, O(1)
(define lex:cons (lambda (x l) (+ (list x) l)))

;; ---- module state ----

(define lex:src "")     ;; source being scanned
(define lex:len 0)
;; Preprocessor mode (L9). Off by default, so a source that has
;; already been through gcc -E lexes exactly as it always did: '#'
;; lines are skipped whole. On, '#' becomes a token, a backslash at
;; end of line splices the next line WITHOUT advancing the line
;; counter (so a multi-line #define stays one logical line, which is
;; how the preprocessor finds where a directive ends), and the header
;; name in #include <...> comes back as one token instead of a dozen.
(define lex:pp false)
(define lex:want-header false)
(define lex:val 0)      ;; mailbox: value from the number scanners
(define lex:oplen 1)    ;; mailbox: bytes consumed by lex:op

;; Conforming escapes (L10). Off by default, so a source lexes exactly
;; as it always did and the c4cc differential battery keeps its
;; premise. On (-conforming), escape sequences mean what C says they
;; mean: \t is 9 not 8, \r is 13 not 10, \a \b \f \v decode, and
;; \xHH / \NNN exist at all -- which is what lets a program write
;; "\033[2J" instead of poking 27 in as an integer.
(define lex:conforming false)

;; ---- character classes (booleans only; see dialect warning) ----

(define lex:identstart? (lambda (c)
	(if (>= c 97) (<= c 122)
	(if (>= c 65) (if (<= c 90) true (= c 95))
	false))))

(define lex:ident? (lambda (c)
	(if (>= c 97) (<= c 122)
	(if (>= c 65) (if (<= c 90) true (= c 95))
	(if (>= c 48) (<= c 57)
	false)))))

(define lex:digit? (lambda (c) (if (>= c 48) (<= c 57) false)))
(define lex:octdigit? (lambda (c) (if (>= c 48) (<= c 55) false)))

;; hex digit value, or -1
(define lex:hexval (lambda (c)
	(if (lex:digit? c) (- c 48)
	(if (if (>= c 97) (<= c 102) false) (- c 87)
	(if (if (>= c 65) (<= c 70) false) (- c 55)
	(- 0 1))))))

;; byte at i, or 0 past the end (c4cc relies on the nul terminator)
(define lex:peek (lambda (i)
	(if (< i lex:len) (string:byte lex:src i) 0)))

;; ---- keywords ----

(define lex:keywords '(
	("char" Char) ("else" Else) ("enum" Enum) ("if" If) ("int" Int)
	("return" Return) ("sizeof" Sizeof) ("while" While)
	("switch" Switch) ("case" Case) ("default" Default) ("break" Break)
	("for" For) ("continue" Continue)
	("static" Static) ("extern" Extern) ("__attribute__" Attribute)
	("constructor" Constructor) ("destructor" Destructor)
	("struct" Struct) ("union" Union) ("typedef" Typedef) ("do" Do)
	("void" Char)))  ;; void IS char in c4, as in c4's own symbol seeding

;; keyword atom, or false
(define lex:kwlook (lambda (kws name)
	(if (empty? kws) false
	(if (= (head (head kws)) name) (index (head kws) 1)
	(next lex:kwlook (tail kws) name)))))

;; ---- scanners: each returns the index after the consumed text ----

;; to end of line, leaving the newline for the main loop's counter
(define lex:hash2? (lambda (i)
	(if (>= i lex:len) false
	(= (string:byte lex:src i) 35))))

(define lex:lparen? (lambda (i)
	(if (>= i lex:len) false
	(= (string:byte lex:src i) 40))))

(define lex:eol? (lambda (i)
	(if (>= i lex:len) false
	(= (string:byte lex:src i) 10))))

;; end of a <...> header name, at the '>'
(define lex:hdrend (lambda (i)
	(if (>= i lex:len) i
	(if (= (string:byte lex:src i) 62) i
	(next lex:hdrend (+ i 1))))))

(define lex:skipline (lambda (i)
	(if (>= i lex:len) i
	(if (= (string:byte lex:src i) 10) i
	(next lex:skipline (+ i 1))))))

;; /* ... */ body; returns (END LINE) since it counts newlines
(define lex:skipblock (lambda (i line)
	(if (>= i lex:len) (list i line)
	(begin
		(define c (string:byte lex:src i))
		(if (= c 10) (next lex:skipblock (+ i 1) (+ line 1))
		(if (if (= c 42) (= (lex:peek (+ i 1)) 47) false)
			(list (+ i 2) line)
		(next lex:skipblock (+ i 1) line)))))))

(define lex:identend (lambda (i)
	(if (>= i lex:len) i
	(if (lex:ident? (string:byte lex:src i)) (next lex:identend (+ i 1))
	i))))

(define lex:decimal (lambda (i v)
	(begin
		(define c (lex:peek i))
		(if (lex:digit? c) (next lex:decimal (+ i 1) (+ (* v 10) (- c 48)))
		(begin (set! lex:val v) i)))))

(define lex:hex (lambda (i v)
	(begin
		(define h (lex:hexval (lex:peek i)))
		(if (>= h 0) (next lex:hex (+ i 1) (+ (* v 16) h))
		(begin (set! lex:val v) i)))))

(define lex:octal (lambda (i v)
	(begin
		(define c (lex:peek i))
		(if (lex:octdigit? c) (next lex:octal (+ i 1) (+ (* v 8) (- c 48)))
		(begin (set! lex:val v) i)))))

;; number starting at i whose first digit is c: nonzero -> decimal,
;; 0x/0X -> hex, else octal (0 alone falls out of the octal loop as 0)
(define lex:number (lambda (i c)
	(if (!= c 48) (lex:decimal (+ i 1) (- c 48))
	(if (if (= (lex:peek (+ i 1)) 120) true (= (lex:peek (+ i 1)) 88))
		(lex:hex (+ i 2) 0)
	(lex:octal (+ i 1) 0)))))

;; index of the closing quote q, treating backslash as consuming the
;; next byte (so \" does not close), exactly as c4cc's scan
(define lex:strend (lambda (i q)
	(if (>= i lex:len) i
	(begin
		(define c (string:byte lex:src i))
		(if (= c q) i
		(if (= c 92) (next lex:strend (+ i 2) q)
		(next lex:strend (+ i 1) q)))))))

;; c4cc's escape table, quirks preserved: \n->10 \t->8 \r->10 \0->0,
;; anything else escaped is itself
(define lex:escape (lambda (c)
	(if (= c 110) 10
	(if (= c 116) 8
	(if (= c 114) 10
	(if (= c 48) 0
	c))))))

;; C limits an octal escape to three digits -- "\0012" is \001 then
;; '2' -- unlike lex:octal, which is a number scanner and runs to the
;; first non-digit. A hex escape really is unbounded in C, so \xHH
;; reuses lex:hex as-is.
(define lex:escoctal (lambda (i n v)
	(begin
		(define c (lex:peek i))
		(if (if (< n 3) (lex:octdigit? c) false)
			(next lex:escoctal (+ i 1) (+ n 1) (+ (* v 8) (- c 48)))
		(begin (set! lex:val v) i)))))

;; The escape at i, where i is the byte AFTER the backslash. Returns
;; the index just past the escape and leaves the decoded byte in the
;; lex:val mailbox -- the same contract as lex:hex/lex:octal, because
;; \xHH and \NNN are literally those scanners. The old fixed-width
;; (+ i 2) in lex:strdecode is what made multi-byte escapes impossible.
(define lex:escape2 (lambda (i)
	(begin
		(define c (lex:peek i))
		(if lex:conforming
			(if (= c 110) (begin (set! lex:val 10) (+ i 1))
			(if (= c 116) (begin (set! lex:val  9) (+ i 1))
			(if (= c 114) (begin (set! lex:val 13) (+ i 1))
			(if (= c  97) (begin (set! lex:val  7) (+ i 1))
			(if (= c  98) (begin (set! lex:val  8) (+ i 1))
			(if (= c 102) (begin (set! lex:val 12) (+ i 1))
			(if (= c 118) (begin (set! lex:val 11) (+ i 1))
			(if (if (= c 120) true (= c 88)) (lex:hex (+ i 1) 0)
			(if (lex:octdigit? c) (lex:escoctal i 0 0)
			(begin (set! lex:val c) (+ i 1)))))))))))
		(begin (set! lex:val (lex:escape c)) (+ i 1))))))

;; decode [i,end) into buf; returns the decoded length
(define lex:strdecode (lambda (i end buf n)
	(if (>= i end) n
	(begin
		(define c (string:byte lex:src i))
		(if (= c 92)
			(begin
				(define j (lex:escape2 (+ i 1)))
				(string:byte! buf n lex:val)
				(next lex:strdecode j end buf (+ n 1)))
		(begin
			(string:byte! buf n c)
			(next lex:strdecode (+ i 1) end buf (+ n 1))))))))

;; operator/punctuation at i (first byte c): kind atom or false;
;; consumed length in the lex:oplen mailbox. Compound assignments and
;; -> are L7 additions; c4cc's lexer never had them.
(define lex:op (lambda (i c)
	(begin
		(define d (lex:peek (+ i 1)))
		(define d2 (lex:peek (+ i 2)))
		(set! lex:oplen 1)
		(if (= c 61) (if (= d 61) (begin (set! lex:oplen 2) 'Eq) 'Assign)
		(if (= c 43) (if (= d 43) (begin (set! lex:oplen 2) 'Inc)
		             (if (= d 61) (begin (set! lex:oplen 2) 'AddA) 'Add))
		(if (= c 45) (if (= d 45) (begin (set! lex:oplen 2) 'Dec)
		             (if (= d 62) (begin (set! lex:oplen 2) 'Arrow)
		             (if (= d 61) (begin (set! lex:oplen 2) 'SubA) 'Sub)))
		(if (= c 33) (if (= d 61) (begin (set! lex:oplen 2) 'Ne) 'Not)
		(if (= c 60) (if (= d 61) (begin (set! lex:oplen 2) 'Le)
		             (if (= d 60)
		                 (if (= d2 61) (begin (set! lex:oplen 3) 'ShlA)
		                     (begin (set! lex:oplen 2) 'Shl))
		             'Lt))
		(if (= c 62) (if (= d 61) (begin (set! lex:oplen 2) 'Ge)
		             (if (= d 62)
		                 (if (= d2 61) (begin (set! lex:oplen 3) 'ShrA)
		                     (begin (set! lex:oplen 2) 'Shr))
		             'Gt))
		(if (= c 124) (if (= d 124) (begin (set! lex:oplen 2) 'Lor)
		              (if (= d 61) (begin (set! lex:oplen 2) 'OrA) 'Or))
		(if (= c 38) (if (= d 38) (begin (set! lex:oplen 2) 'Lan)
		             (if (= d 61) (begin (set! lex:oplen 2) 'AndA) 'And))
		(if (= c 94) (if (= d 61) (begin (set! lex:oplen 2) 'XorA) 'Xor)
		(if (= c 37) (if (= d 61) (begin (set! lex:oplen 2) 'ModA) 'Mod)
		(if (= c 42) (if (= d 61) (begin (set! lex:oplen 2) 'MulA) 'Mul)
		(if (= c 91) 'Brak
		(if (= c 63) 'Cond
		(if (= c 126) 'Tilde
		(if (= c 59) 'Semi
		(if (= c 58) 'Colon
		(if (= c 44) 'Comma
		(if (= c 40) 'Lparen
		(if (= c 41) 'Rparen
		(if (= c 123) 'Lbrace
		(if (= c 125) 'Rbrace
		(if (= c 93) 'Rbrak
		(if (= c 46) 'Dot
		false))))))))))))))))))))))))))

;; ---- main loop ----

(define lex:go (lambda (i line acc)
	(if (>= i lex:len)
		(lex:reverse (lex:cons (list 'Eof 0 line) acc))
	(begin
		(define c (string:byte lex:src i))
		(define j (+ i 1))
		(if (if (= c 92) (lex:eol? j) false)
			;; backslash-newline: splice, same logical line
			(next lex:go (+ j 1) line acc)
		(if (= c 10)
			(begin
				(set! lex:want-header false)
				(next lex:go j (+ line 1) acc))
		(if (<= c 32) (next lex:go j line acc)
		(if (= c 35)
			(if lex:pp
				;; '##' is one token: the paste operator. A lone '#'
				;; is either a directive introducer or stringize --
				;; the preprocessor tells them apart by position.
				(if (lex:hash2? j)
					(next lex:go (+ j 1) line (lex:cons (list 'HashHash 0 line) acc))
				(next lex:go j line (lex:cons (list 'Hash 0 line) acc)))
			(next lex:go (lex:skipline j) line acc))
		(if (if lex:want-header (= c 60) false)
			;; #include <name>: one token, not a stream of operators
			(begin
				(define e (lex:hdrend j))
				(set! lex:want-header false)
				(next lex:go (+ e 1) line (lex:cons
					(list 'Str (string:substr lex:src j (- e j)) line) acc)))
		(if (lex:identstart? c)
			(begin
				(define e (lex:identend j))
				(define name (string:substr lex:src i (- e i)))
				(define kw (lex:kwlook lex:keywords name))
				;; arm the <...> header scan; cleared at end of line,
				;; so it only bites on an actual #include line
				(if lex:pp
					(if (= name "include") (set! lex:want-header true) nil) nil)
				;; In pp mode an identifier carries a fourth field: 1
				;; when '(' TOUCHES it. That single bit is what
				;; separates "#define ADD(a,b) ..." (function-like)
				;; from "#define TWO (x + y)" (object-like whose body
				;; happens to start with a paren) -- the standard
				;; draws the line at the space, so the lexer must be
				;; the one to notice it.
				(next lex:go e line (lex:cons
					(if kw (list kw 0 line)
						(if lex:pp
							(list 'Id name line (if (lex:lparen? e) 1 0))
						(list 'Id name line))) acc)))
		(if (lex:digit? c)
			(begin
				(define e (lex:number i c))
				(next lex:go e line
					(lex:cons (list 'Num lex:val line) acc)))
		(if (= c 47)
			(if (= (lex:peek j) 47)
				(next lex:go (lex:skipline (+ j 1)) line acc)
			(if (= (lex:peek j) 42)
				(begin
					(define r (lex:skipblock (+ j 1) line))
					(next lex:go (head r) (index r 1) acc))
			(if (= (lex:peek j) 61)
				(next lex:go (+ j 1) line (lex:cons (list 'DivA 0 line) acc))
			(next lex:go j line (lex:cons (list 'Div 0 line) acc)))))
		(if (= c 34)
			(begin
				(define e (lex:strend j 34))
				(define buf (string:alloc (- e j)))
				(define n (lex:strdecode j e buf 0))
				(next lex:go (+ e 1) line (lex:cons
					(list 'Str (string:substr buf 0 n) line) acc)))
		(if (= c 39)
			(begin
				(define e (lex:strend j 39))
				(define buf (string:alloc (- e j)))
				(define n (lex:strdecode j e buf 0))
				(next lex:go (+ e 1) line (lex:cons
					(list 'Num
						(if (> n 0) (string:byte buf (- n 1)) 0)
						line) acc)))
		(begin
			(define k (lex:op i c))
			(if k
				(next lex:go (+ i lex:oplen) line
					(lex:cons (list k 0 line) acc))
			(next lex:go j line acc)))))))))))))))))  ;; unknown byte: skip, as c4cc

;; ---- entry points ----

(define lex:string (lambda (src)
	(begin
		(set! lex:src src)
		(set! lex:len (length src))
		(lex:go 0 1 (list)))))

(define lex:file (lambda (path)
	(lex:string (file:read (file:path path)))))

)
