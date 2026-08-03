;; c4lc.lisp -- driver: compile a C file to a .c4r image (L2)
;;
;;   c4sp c4lc.lisp in.c out.c4r
;;
;; Lex, parse, generate, encode, write. file:write uses the C4KE RAM
;; filesystem when present, so this also works inside C4KE.
(begin
	(load "c4lc-lex.lisp")
	(load "c4lc-parse.lisp")
	(load "c4lc-gen.lisp")
	(load "c4r.lisp")
	(if (< (length argv) 2) (error "usage: c4lc.lisp in.c out.c4r"))
	(define In (head argv))
	(define OutName (index argv 1))
	(define M (gen:module (parse:program (lex:file In))))
	(define Out (c4r:encode M))
	(if (file:write OutName Out)
		(print ";; c4lc:" In "-" (length Out) "bytes -" OutName)
		(error "c4lc: write failed")))
