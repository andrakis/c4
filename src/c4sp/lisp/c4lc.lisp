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
	(define Cisc false)        ;; -mcisc: emit c4mp-only fused opcodes (M12)
	(define Fuse false)        ;; -mfuse: emit the fused opcodes
	(define Pp false)          ;; -P: preprocess internally (L9)
	(define Conf false)        ;; -conforming: real C escapes (L10)
	(define Paths nil)
	(define PreDefs nil)
	(define Args argv)
	(define flags (lambda ()
		(if (empty? Args) nil
		(if (= (+ "" (head Args)) "-O")
			(begin (set! Opt true) (set! Args (tail Args)) (next flags))
		(if (= (+ "" (head Args)) "-c")
			(begin (set! Obj true) (set! Args (tail Args)) (next flags))
		;; -mcisc: emit LXI/SXI (and any future fused opcodes) for
		;; array-of-8-byte-element indexing instead of the generic
		;; IMM/PSH/SHL/ADD/LI-or-SI sequence. Only c4mp understands
		;; them -- c4m traps them as an illegal opcode (see c4mp.h) --
		;; so code compiled with this flag must be run under c4mp, not
		;; c4m. Off by default: this is why the flag exists at all,
		;; rather than emitting them unconditionally the way c4mp's
		;; own processor opcodes are (those are explicit function
		;; calls a program opts into; LXI/SXI would otherwise silently
		;; change every array access in every c4lc-compiled program).
		(if (= (+ "" (head Args)) "-mcisc")
			(begin (set! Cisc true) (set! Args (tail Args)) (next flags))
		;; -mfuse: run c4opt's fuse pass, which rewrites the two- and
		;; three-instruction sequences a third of the instructions real
		;; workloads execute are made of into single opcodes
		;; (docs/fused-opcodes.md). c4m, c4mp and oisc4 have them;
		;; PLAIN C4 DOES NOT, so an image built with this does not run
		;; under ./c4 or ./c4 c4l.c. Off by default for exactly that
		;; reason, and it implies -O since the pass lives in c4opt.
		(if (= (+ "" (head Args)) "-mfuse")
			(begin (set! Fuse true) (set! Opt true) (set! Args (tail Args)) (next flags))
		;; -P runs c4lc's own preprocessor instead of expecting a
		;; source that gcc -E has already been through. -I adds an
		;; include directory, -D predefines a macro.
		;; -conforming: decode escape sequences the way C defines them
		;; instead of reproducing c4cc's table (\t->8, \r->10, no
		;; \xHH or \NNN). Off by default for the same reason -mcisc
		;; is: the c4cc differential battery compiles the SAME source
		;; with both compilers, and the L0 lexer golden pins the quirks
		;; deliberately. A program opts in, and then "\033[2J" works.
		(if (= (+ "" (head Args)) "-conforming")
			(begin (set! Conf true) (set! Args (tail Args)) (next flags))
		(if (= (+ "" (head Args)) "-P")
			(begin (set! Pp true) (set! Args (tail Args)) (next flags))
		(if (= (+ "" (head Args)) "-I")
			(begin
				(set! Pp true)
				(set! Paths (+ Paths (list (+ "" (index Args 1)))))
				(set! Args (tail (tail Args)))
				(next flags))
		(if (= (+ "" (head Args)) "-D")
			(begin
				(set! Pp true)
				(set! PreDefs (+ PreDefs (list (+ "" (index Args 1)))))
				(set! Args (tail (tail Args)))
				(next flags))
		nil)))))))))))
	(flags)
	(if (< (length Args) 2) (error "usage: c4lc.lisp [-O] [-c] in.c out"))
	(define In (head Args))
	(define OutName (index Args 1))
	(set! lex:conforming Conf)
	(set! gen:objmode Obj)
	(set! gen:cisc Cisc)
	(define Ast (parse:program
		(if Pp
			(begin
				(load "c4lc-pp.lisp")
				(set! pp:paths Paths)
				(pp:predefines PreDefs)
				(pp:file In))
		(lex:file In))))
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
			;; after the load, because that is where opt:fuse-on is
			;; defined
			(set! opt:fuse-on Fuse)
			(set! M (c4opt:optimize M)))
		nil)
	;; Format v3 for both objects and whole-program images: uninitialized
	;; globals are segregated to BSS (gen:bssextra), which occupies no
	;; image bytes and is zero-filled at load. c4rlink accumulates each
	;; object's MEMSZ when merging, so objects carry it too.
	(set! c4r:v3 true)
	(set! c4r:bss-extra gen:bssextra)
	(define Out (c4r:encode M))
	(set! c4r:bss-extra 0)
	(if (file:write OutName Out)
		(print ";; c4lc:" In "-" (length Out) "bytes -" OutName)
		(error "c4lc: write failed")))
