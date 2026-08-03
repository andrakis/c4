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
(define lex:val 0)      ;; mailbox: value from the number scanners
(define lex:oplen 1)    ;; mailbox: bytes consumed by lex:op

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

;; decode [i,end) into buf; returns the decoded length
(define lex:strdecode (lambda (i end buf n)
	(if (>= i end) n
	(begin
		(define c (string:byte lex:src i))
		(if (= c 92)
			(begin
				(string:byte! buf n (lex:escape (lex:peek (+ i 1))))
				(next lex:strdecode (+ i 2) end buf (+ n 1)))
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
		(if (= c 10) (next lex:go j (+ line 1) acc)
		(if (<= c 32) (next lex:go j line acc)
		(if (= c 35) (next lex:go (lex:skipline j) line acc)
		(if (lex:identstart? c)
			(begin
				(define e (lex:identend j))
				(define name (string:substr lex:src i (- e i)))
				(define kw (lex:kwlook lex:keywords name))
				(next lex:go e line (lex:cons
					(if kw (list kw 0 line) (list 'Id name line)) acc)))
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
			(next lex:go j line acc)))))))))))))))  ;; unknown byte: skip, as c4cc

;; ---- entry points ----

(define lex:string (lambda (src)
	(begin
		(set! lex:src src)
		(set! lex:len (length src))
		(lex:go 0 1 (list)))))

(define lex:file (lambda (path)
	(lex:string (file:read (file:path path)))))

)
