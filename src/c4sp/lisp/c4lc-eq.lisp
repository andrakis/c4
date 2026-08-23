;; c4lc-eq.lisp -- compile in memory and compare against a reference
;; image (L5): proves c4lc's output is host-independent -- the same
;; bytes whether the interpreter is the native c4sp build, c4sp.c4r
;; under c4m, or the interpreter c4lc compiled itself. Nothing is
;; written, so this runs under the bare VM (which has no write
;; syscall).
;;
;;   c4sp c4lc-eq.lisp in.c reference.c4r
(begin
	(load "c4lc-lex.lisp")
	(load "c4lc-parse.lisp")
	(load "c4lc-gen.lisp")
	(load "c4r.lisp")
	(if (< (length argv) 2) (error "usage: c4lc-eq.lisp in.c ref.c4r"))
	;; Encode exactly as c4lc.lisp does. The driver segregates
	;; uninitialized globals to BSS and stamps format v3; encoding here
	;; without doing the same compares a v2 image against a v3 one and
	;; reports a one-byte difference that has nothing to do with host
	;; independence, which is all this test is meant to measure.
	(define M (gen:module (parse:program (lex:file (head argv)))))
	(set! c4r:v3 true)
	(set! c4r:bss-extra gen:bssextra)
	(define Out (c4r:encode M))
	(define Ref (file:read (file:path (index argv 1))))
	(if (= Out Ref)
		(print "c4lc fixed point:" (head argv) "-" (length Out) "bytes identical")
		(begin
			(print "c4lc fixed point DIFFERS:" (length Out) "vs" (length Ref) "bytes")
			(error "fixed point failed"))))
