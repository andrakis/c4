;; c4lc-tokens.lisp -- L0 driver: dump a C file's token stream
;;
;;   c4sp c4lc-tokens.lisp file.c          one (KIND VALUE LINE) per line
;;   c4sp c4lc-tokens.lisp -count file.c   token count only (benchmarks)
;;   c4sp c4lc-tokens.lisp -conforming f.c  decode escapes as C defines
;;                                          them rather than as c4cc does
(begin
	(load "c4lc-lex.lisp")
	(if (empty? argv) (error "usage: c4lc-tokens.lisp [-count] file.c"))
	;; argv elements are atoms, not strings; stringify for the compare
	(define flag (+ "" (head argv)))
	(define counting (= flag "-count"))
	(set! lex:conforming (= flag "-conforming"))
	(define file (if (if counting true lex:conforming) (index argv 1) (head argv)))
	(define toks (lex:file file))
	(define dump (lambda (l)
		(if (empty? l) nil
		(begin
			(print (head l))
			(next dump (tail l))))))
	(if counting
		(print "tokens" (length toks))
		(dump toks)))
