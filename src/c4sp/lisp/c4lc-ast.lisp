;; c4lc-ast.lisp -- L1 driver: parse a C file and dump its AST
;;
;;   c4sp c4lc-ast.lisp file.c          one top-level decl per line
;;   c4sp c4lc-ast.lisp -check file.c   "parse ok FILE (N decls)" only
(begin
	(load "c4lc-lex.lisp")
	(load "c4lc-parse.lisp")
	(if (empty? argv) (error "usage: c4lc-ast.lisp [-check] file.c"))
	(define checking (= (+ "" (head argv)) "-check"))
	(define file (if checking (index argv 1) (head argv)))
	(define ast (parse:program (lex:file file)))
	(define dump (lambda (l)
		(if (empty? l) nil
		(begin
			(print (head l))
			(next dump (tail l))))))
	(if checking
		(print "parse ok" file (+ "(" (length (tail ast)) " decls)"))
		(dump (tail ast))))
