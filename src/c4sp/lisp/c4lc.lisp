;; c4lc.lisp -- driver: compile a C file to a .c4r image (L2; -O is L4)
;;
;;   c4sp c4lc.lisp [-O] in.c out.c4r
;;
;; Lex, parse, generate, optionally run the c4opt passes in-process
;; (no intermediate file -- the module IS the optimizer's input form),
;; encode, write. file:write uses the C4KE RAM filesystem when
;; present, so this also works inside C4KE.
(begin
	(load "c4lc-lex.lisp")
	(load "c4lc-parse.lisp")
	(load "c4lc-gen.lisp")
	(load "c4r.lisp")
	(define Opt (if (empty? argv) false (= (+ "" (head argv)) "-O")))
	(define Args (if Opt (tail argv) argv))
	(if (< (length Args) 2) (error "usage: c4lc.lisp [-O] in.c out.c4r"))
	(define In (head Args))
	(define OutName (index Args 1))
	(define M (gen:module (parse:program (lex:file In))))
	(if Opt
		(begin
			(load "c4opt.lisp")
			(set! M (c4opt:optimize M)))
		nil)
	(define Out (c4r:encode M))
	(if (file:write OutName Out)
		(print ";; c4lc:" In "-" (length Out) "bytes -" OutName)
		(error "c4lc: write failed")))
