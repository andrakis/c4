;; c4lc.lisp -- driver: compile a C file to a .c4r image or .c4o object
;;
;;   c4sp c4lc.lisp [-O] [-c] in.c out
;;
;; -O runs the tree passes and the c4opt peephole passes in-process.
;; -c compiles ONE UNIT to an object: prototypes with no definition
;;    become extern symbols whose call sites carry SYMBOL-typed
;;    patches, main is optional (entry -1 without it), and c4rlink
;;    resolves the rest. Without -c, undefined prototypes become
;;    exit-255 stubs and main is required.
;;
;; file:write uses the C4KE RAM filesystem when present, so this also
;; works inside C4KE.
(begin
	(load "c4lc-lex.lisp")
	(load "c4lc-parse.lisp")
	(load "c4lc-gen.lisp")
	(load "c4r.lisp")
	(define Opt false)
	(define Obj false)
	(define Args argv)
	(define flags (lambda ()
		(if (empty? Args) nil
		(if (= (+ "" (head Args)) "-O")
			(begin (set! Opt true) (set! Args (tail Args)) (next flags))
		(if (= (+ "" (head Args)) "-c")
			(begin (set! Obj true) (set! Args (tail Args)) (next flags))
		nil)))))
	(flags)
	(if (< (length Args) 2) (error "usage: c4lc.lisp [-O] [-c] in.c out"))
	(define In (head Args))
	(define OutName (index Args 1))
	(set! gen:objmode Obj)
	(define Ast (parse:program (lex:file In)))
	(if Opt
		(begin
			;; L6 tree passes first (fold, dead branches, dead
			;; functions), then generate, then the peephole passes
			(load "c4lc-tree.lisp")
			(set! tree:objmode Obj)
			(set! Ast (tree:optimize Ast)))
		nil)
	(define M (gen:module Ast))
	(if Opt
		(begin
			(load "c4opt.lisp")
			(set! M (c4opt:optimize M)))
		nil)
	(define Out (c4r:encode M))
	(if (file:write OutName Out)
		(print ";; c4lc:" In "-" (length Out) "bytes -" OutName)
		(error "c4lc: write failed")))
