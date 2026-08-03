;; c4lc-tokens.lisp -- L0 driver: dump a C file's token stream
;;
;;   c4sp c4lc-tokens.lisp file.c          one (KIND VALUE LINE) per line
;;   c4sp c4lc-tokens.lisp -count file.c   token count only (benchmarks)
(begin
	(load "c4lc-lex.lisp")
	(if (empty? argv) (error "usage: c4lc-tokens.lisp [-count] file.c"))
	;; argv elements are atoms, not strings; stringify for the compare
	(define counting (= (+ "" (head argv)) "-count"))
	(define file (if counting (index argv 1) (head argv)))
	(define toks (lex:file file))
	(define dump (lambda (l)
		(if (empty? l) nil
		(begin
			(print (head l))
			(next dump (tail l))))))
	(if counting
		(print "tokens" (length toks))
		(dump toks)))
