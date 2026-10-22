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
	("float" Float) ("double" Double)
	;; void IS char in c4, as in c4's own symbol seeding. It has a kind
	;; of its own only so that a parameter list can tell `(void)` from
	;; `(char)`; the parser takes Void wherever it takes Char.
	("void" Void)))

;; The M4 type words (docs/c4lc-design.md, "Integer types"). Kept in a
;; table of their own because without -P they are keywords only when
;; the program asks (lex:file's caller sets it up; see below).
;;
;; In -P mode EVERY word lexes as Id, these and lex:keywords alike, and
;; pp:kwfix turns the survivors back into keywords after expansion --
;; the order C's own phases put it in, and c4fc's (lex.f T-IDENT). A
;; keyword is a macro name like any other word: include/stddef.h says
;; `#ifndef long` / `#define long int`, src/tests/factorial.c says
;; `#define int long long`, and `#x` of a keyword is its spelling.
(define lex:keywords2 '(
	("const" Const) ("volatile" Volatile) ("signed" Signed)
	("unsigned" Unsigned) ("short" Short) ("long" Long)))

;; keyword atom, or false
(define lex:kwlook (lambda (kws name)
	(if (empty? kws) false
	(if (= (head (head kws)) name) (index (head kws) 1)
	(next lex:kwlook (tail kws) name)))))

;; Integer suffixes: u/U makes the literal unsigned, l/L (any number of
;; them) widens it. Only the unsigned bit changes anything -- long is
;; already a word -- so the result is 0 (int), 1 (unsigned int) or 2
;; (unsigned long), left in the lex:sfx mailbox like lex:val.
(define lex:sfx 0)
(define lex:nl 0)       ;; how many l/L: two is long long
(define lex:suffix (lambda (i u l)
	(begin
		(define c (lex:peek i))
		(if (if (= c 117) true (= c 85)) (next lex:suffix (+ i 1) 1 l)
		(if (if (= c 108) true (= c 76)) (next lex:suffix (+ i 1) u (+ l 1))
		(begin
			(set! lex:sfx (if (= u 0) 0 (if (= l 0) 1 2)))
			(set! lex:nl l)
			i))))))

;; A literal on a 32-bit host (M8) is read again in four 16-bit limbs, which
;; a 32-bit word carries without overflow; one that says ll is a NumLL
;; (NumULL with a u) whose value is (LO HI). Without the suffix a literal
;; keeps c4's rule (14.3) however large: code written for the 64-bit word,
;; 0x7FFFFFFFFFFFFFFF in c4sp's own float library, wraps to a word here.
(define lex:wlo 0)
(define lex:whi 0)
(define lex:wide (lambda (i e base l0 l1 l2 l3)
	(if (>= i e)
		(begin
			(set! lex:wlo (bit:or l0 (bit:shl l1 16)))
			(set! lex:whi (bit:or l2 (bit:shl l3 16)))
			(>= lex:nl 2))
	(begin
		(define x0 (+ (* l0 base) (lex:hexval (string:byte lex:src i))))
		(define x1 (+ (* l1 base) (bit:shr x0 16)))
		(define x2 (+ (* l2 base) (bit:shr x1 16)))
		(define x3 (+ (* l3 base) (bit:shr x2 16)))
		(next lex:wide (+ i 1) e base
			(bit:and x0 65535) (bit:and x1 65535) (bit:and x2 65535) (bit:and x3 65535))))))
(define lex:wide? (lambda (i c e)
	(if (!= (sys:wordsize) 4) false
	(if (!= c 48) (lex:wide i e 10 0 0 0 0)
	(if (if (= (lex:peek (+ i 1)) 120) true (= (lex:peek (+ i 1)) 88))
		(lex:wide (+ i 2) e 16 0 0 0 0)
	(lex:wide (+ i 1) e 8 0 0 0 0))))))

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

;; #pragma pack with lex:pp off is not skipped: it becomes a PragmaPack
;; token, and the rest of its line, `(push, 2)` and all, lexes as usual
;; for p:pragmapack. lex:pragmapack is the index after `pragma pack`, or
;; false.
(define lex:blanks (lambda (i)
	(if (>= i lex:len) i
	(if (if (= (string:byte lex:src i) 32) true (= (string:byte lex:src i) 9))
		(next lex:blanks (+ i 1))
	i))))
(define lex:word (lambda (i w)
	(begin
		(define s (lex:blanks i))
		(define e (lex:identend s))
		(if (= (+ "" (string:substr lex:src s (- e s))) w) e false))))
(define lex:pragmapack (lambda (i)
	(begin
		(define a (lex:word i "pragma"))
		(if a (lex:word a "pack") false))))

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

;; ---- floating-point literals (docs/c4-float.md) ----
;;
;; 1.5, .5, 1., 1e-3, 2.5f: a decimal literal with a point or an
;; exponent. It is a double unless it says f (float); l says double too.
;; The lexer turns it into its IEEE bits here, correctly rounded (to
;; nearest, ties to even, subnormals and all), so the parser and the
;; generator only ever see bit patterns: FNumF is a float's word, FNumD
;; a double's two 32-bit halves (LO HI). Hex floats are not read.
;;
;; The arithmetic is exact, on big integers held as lists of 16-bit
;; limbs, lowest first -- a 32-bit c4sp has no wider number than its
;; word, and this must give the same bits on both. The value is
;; DIGITS * 10^E10 = A / B; scaled by 2^s until the quotient has P+2
;; bits, it is divided one bit at a time, and the remainder is the
;; sticky bit.

(define lex:brev (lambda (l acc) (if (empty? l) acc (next lex:brev (tail l) (lex:cons (head l) acc)))))
(define lex:blimb (lambda (a i)
	(if (empty? a) 0 (if (= i 0) (head a) (next lex:blimb (tail a) (- i 1))))))
;; a * m + c, with m and c small
(define lex:bmul (lambda (a m c acc)
	(if (empty? a) (lex:brev (if (= c 0) acc (lex:cons c acc)) (list))
	(begin
		(define x (+ (* (head a) m) c))
		(next lex:bmul (tail a) m (bit:shr x 16) (lex:cons (bit:and x 65535) acc))))))
(define lex:bpow10 (lambda (a n) (if (<= n 0) a (next lex:bpow10 (lex:bmul a 10 0 (list)) (- n 1)))))
(define lex:bzeros (lambda (w a) (if (<= w 0) a (next lex:bzeros (- w 1) (lex:cons 0 a)))))
(define lex:bshlb (lambda (a s c acc)
	(if (empty? a) (lex:brev (if (= c 0) acc (lex:cons c acc)) (list))
	(begin
		(define x (bit:or (bit:shl (head a) s) c))
		(next lex:bshlb (tail a) s (bit:shr x 16) (lex:cons (bit:and x 65535) acc))))))
(define lex:bshl (lambda (a n)
	(begin
		(define w (/ n 16))
		(lex:bzeros w (lex:bshlb a (- n (* w 16)) 0 (list))))))
(define lex:bdrop (lambda (a w) (if (<= w 0) a (if (empty? a) a (next lex:bdrop (tail a) (- w 1))))))
(define lex:bshrb (lambda (a s acc)
	(if (empty? a) (lex:brev acc (list))
	(begin
		(define nx (if (empty? (tail a)) 0 (head (tail a))))
		(next lex:bshrb (tail a) s
			(lex:cons (bit:and (bit:or (bit:shr (head a) s) (bit:shl nx (- 16 s))) 65535) acc))))))
(define lex:bshr (lambda (a n)
	(begin
		(define w (/ n 16))
		(lex:bshrb (lex:bdrop a w) (- n (* w 16)) (list)))))
(define lex:bits16 (lambda (x n) (if (= x 0) n (next lex:bits16 (bit:shr x 1) (+ n 1)))))
(define lex:bbitlen (lambda (a) (lex:bbitlen/2 a 0 0)))
(define lex:bbitlen/2 (lambda (a i best)
	(if (empty? a) best
	(next lex:bbitlen/2 (tail a) (+ i 1)
		(if (= (head a) 0) best (+ (* i 16) (lex:bits16 (head a) 0)))))))
(define lex:btake (lambda (a n acc)
	(if (<= n 0) acc
	(next lex:btake (if (empty? a) a (tail a)) (- n 1) (lex:cons (if (empty? a) 0 (head a)) acc)))))
;; highest limb first, the same length
(define lex:bcmpr (lambda (a b)
	(if (empty? a) 0
	(if (< (head a) (head b)) -1
	(if (> (head a) (head b)) 1
	(next lex:bcmpr (tail a) (tail b)))))))
(define lex:bcmp (lambda (a b)
	(begin
		(define la (lex:bbitlen a))
		(define lb (lex:bbitlen b))
		(if (< la lb) -1
		(if (> la lb) 1
		(begin
			(define n (/ (+ la 15) 16))
			;; lex:btake leaves the limbs highest first
			(lex:bcmpr (lex:btake a n (list)) (lex:btake b n (list)))))))))
;; a - b, a >= b
(define lex:bsub (lambda (a b brw acc)
	(if (empty? a) (lex:brev acc (list))
	(begin
		(define x (- (- (head a) (if (empty? b) 0 (head b))) brw))
		(next lex:bsub (tail a) (if (empty? b) b (tail b)) (if (< x 0) 1 0)
			(lex:cons (if (< x 0) (+ x 65536) x) acc))))))
(define lex:bbit (lambda (a k)
	(bit:and (bit:shr (lex:blimb a (/ k 16)) (- k (* 16 (/ k 16)))) 1)))
;; any bit set below bit k?
(define lex:blow? (lambda (a k)
	(if (<= k 0) false (!= (lex:bcmp (lex:bshl (lex:bshr a k) k) a) 0))))
(define lex:bword (lambda (a i)
	(bit:or (lex:blimb a (* 2 i)) (bit:shl (lex:blimb a (+ (* 2 i) 1)) 16))))
(define lex:bdigits (lambda (s i acc)
	(if (>= i (length s)) acc
	(next lex:bdigits s (+ i 1) (lex:bmul acc 10 (- (string:byte s i) 48) (list))))))

;; DIGITS * 10^E10 in a format of P significant bits, exponents EMIN..EMAX:
;; (BIASEDEXP FRACTION), the fraction a big integer below 2^(P-1)
(define lex:fpconv (lambda (ds e10 p emin emax)
	(begin
		(define m (lex:bdigits ds 0 (list 0)))
		(define nd (length ds))
		(if (= (lex:bbitlen m) 0) (list 0 (list 0))
		;; far out of range either way: skip the huge powers of ten
		(if (> (+ e10 nd) 400) (list (+ (* 2 emax) 1) (list 0))
		(if (< (+ e10 nd) -400) (list 0 (list 0))
		(begin
			(define a (if (>= e10 0) (lex:bpow10 m e10) m))
			(define b (if (>= e10 0) (list 1) (lex:bpow10 (list 1) (- 0 e10))))
			(lex:fpscale a b (- (+ p 1) (- (lex:bbitlen a) (lex:bbitlen b))) p emin emax))))))))
(define lex:fpscale (lambda (a b s p emin emax)
	(begin
		(define a2 (if (>= s 0) (lex:bshl a s) a))
		(define b2 (if (>= s 0) b (lex:bshl b (- 0 s))))
		(if (>= (lex:bcmp a2 (lex:bshl b2 (+ p 2))) 0) (next lex:fpscale a b (- s 1) p emin emax)
		(if (< (lex:bcmp a2 (lex:bshl b2 (+ p 1))) 0) (next lex:fpscale a b (+ s 1) p emin emax)
		(lex:fpdiv a2 b2 (+ p 1) (list 0) s p emin emax))))))
;; the quotient, a bit at a time from bit I down
(define lex:fpdiv (lambda (a b i q s p emin emax)
	(if (< i 0) (lex:fpround q (> (lex:bbitlen a) 0) (- (+ p 1) s) p emin emax)
	(begin
		(define bi (lex:bshl b i))
		(if (>= (lex:bcmp a bi) 0)
			(next lex:fpdiv (lex:bsub a bi 0 (list)) b (- i 1) (lex:bmul q 2 1 (list)) s p emin emax)
		(next lex:fpdiv a b (- i 1) (lex:bmul q 2 0 (list)) s p emin emax))))))
;; Q has P+2 bits and the value is Q * 2^(E-P-1): round it into the format
(define lex:fpround (lambda (q sticky e p emin emax)
	(if (> e emax) (list (+ (* 2 emax) 1) (list 0))
	(if (>= e emin)
		(begin
			(define keep (lex:bshr q 2))
			(define up (if (= (lex:bbit q 1) 1)
				(if (= (lex:bbit q 0) 1) true (if sticky true (= (lex:bbit keep 0) 1))) false))
			(define k2 (if up (lex:bmul keep 1 1 (list)) keep))
			(if (> (lex:bbitlen k2) p)
				(if (>= e emax) (list (+ (* 2 emax) 1) (list 0))
				(list (+ (+ e 1) emax) (list 0)))
			(list (+ e emax) (lex:bsub k2 (lex:bshl (list 1) (- p 1)) 0 (list)))))
	(begin
		;; subnormal: the units are 2^(EMIN-P+1)
		(define drop (+ 2 (- emin e)))
		(define keep (lex:bshr q drop))
		(define up (if (= (lex:bbit q (- drop 1)) 1)
			(if (lex:blow? q (- drop 1)) true (if sticky true (= (lex:bbit keep 0) 1))) false))
		(define k2 (if up (lex:bmul keep 1 1 (list)) keep))
		(if (>= (lex:bbitlen k2) p) (list 1 (list 0))
		(list 0 k2)))))))

(define lex:fpbits32 (lambda (ds e10)
	(begin
		(define r (lex:fpconv ds e10 24 -126 127))
		(bit:or (bit:shl (head r) 23) (lex:bword (index r 1) 0)))))
;; (LO HI)
(define lex:fpbits64 (lambda (ds e10)
	(begin
		(define r (lex:fpconv ds e10 53 -1022 1023))
		(list (lex:bword (index r 1) 0)
			(bit:or (bit:shl (head r) 20) (lex:bword (index r 1) 1))))))

;; the scan: does a float literal start at i (a digit, or a point and a
;; digit)? Its digits, power of ten and suffix go to the mailboxes below
(define lex:fds "")
(define lex:fe10 0)
(define lex:ff false)
(define lex:digend (lambda (i) (if (lex:digit? (lex:peek i)) (next lex:digend (+ i 1)) i)))
(define lex:expat? (lambda (k)
	(if (if (= (lex:peek k) 101) true (= (lex:peek k) 69))
		(if (lex:digit? (lex:peek (+ k 1))) true
		(if (if (= (lex:peek (+ k 1)) 43) true (= (lex:peek (+ k 1)) 45))
			(lex:digit? (lex:peek (+ k 2))) false))
	false)))
(define lex:fpstart? (lambda (i c)
	(if (= c 46) (lex:digit? (lex:peek (+ i 1)))
	(if (if (= c 48) (if (= (lex:peek (+ i 1)) 120) true (= (lex:peek (+ i 1)) 88)) false) false
	(begin
		(define k (lex:digend i))
		(if (= (lex:peek k) 46) true (lex:expat? k)))))))
(define lex:fpscan (lambda (i)
	(begin
		(define k (lex:digend i))
		(define ip (string:substr lex:src i (- k i)))
		(define fs (if (= (lex:peek k) 46) (+ k 1) k))
		(define fe (lex:digend fs))
		(define fp (string:substr lex:src fs (- fe fs)))
		(define x0 (if (lex:expat? fe) (+ fe 1) fe))
		(define neg (= (lex:peek x0) 45))
		(define x1 (if (if (= (lex:peek x0) 43) true neg) (+ x0 1) x0))
		(define x2 (if (= x0 fe) x1 (lex:decimal x1 0)))
		(define ev (if (= x0 fe) 0 (if neg (- 0 lex:val) lex:val)))
		(define sc (lex:peek x2))
		(set! lex:ff (if (= sc 102) true (= sc 70)))
		(set! lex:fds (lex:fpstrip (+ ip fp) 0))
		(set! lex:fe10 (- ev (length fp)))
		(if (if lex:ff true (if (= sc 108) true (= sc 76))) (+ x2 1) x2))))
;; the digits less their leading zeros ("0" stays a zero)
(define lex:fpstrip (lambda (s i)
	(if (>= i (length s)) "0"
	(if (= (string:byte s i) 48) (next lex:fpstrip s (+ i 1))
	(string:substr s i (- (length s) i))))))
(define lex:fptoken (lambda (line)
	(if lex:ff (list 'FNumF (lex:fpbits32 lex:fds lex:fe10) line)
	(list 'FNumD (lex:fpbits64 lex:fds lex:fe10) line))))

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
			(begin
				(define pk (lex:pragmapack j))
				(if pk (next lex:go pk line (lex:cons (list 'PragmaPack 0 line) acc))
				(next lex:go (lex:skipline j) line acc))))
		(if (if lex:want-header (= c 60) false)
			;; #include <name>: one token, not a stream of operators. Its
			;; fourth field, 1, tells the preprocessor it was <...>, which
			;; is searched along -I only, never beside the including file.
			(begin
				(define e (lex:hdrend j))
				(set! lex:want-header false)
				(next lex:go (+ e 1) line (lex:cons
					(list 'Str (string:substr lex:src j (- e j)) line 1) acc)))
		(if (lex:identstart? c)
			(begin
				(define e (lex:identend j))
				(define name (string:substr lex:src i (- e i)))
				(define kw (if lex:pp false
					(if (lex:kwlook lex:keywords name) (lex:kwlook lex:keywords name)
						(lex:kwlook lex:keywords2 name))))
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
		(if (lex:fpstart? i c)
			(begin
				(define fend (lex:fpscan i))
				(next lex:go fend line (lex:cons (lex:fptoken line) acc)))
		(if (lex:digit? c)
			(begin
				(define e0 (lex:number i c))
				(define e (lex:suffix e0 0 0))
				(next lex:go e line
					(lex:cons
						(if (lex:wide? i c e0)
							(list (if (= lex:sfx 0) 'NumLL 'NumULL) (list lex:wlo lex:whi) line)
						(list (if (= lex:sfx 0) 'Num (if (= lex:sfx 1) 'NumU 'NumUL))
							lex:val line)) acc)))
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
			(next lex:go j line acc))))))))))))))))))  ;; unknown byte: skip, as c4cc

;; ---- entry points ----

(define lex:string (lambda (src)
	(begin
		(set! lex:src src)
		(set! lex:len (length src))
		(lex:go 0 1 (list)))))

(define lex:file (lambda (path)
	(lex:string (file:read (file:path path)))))

)
