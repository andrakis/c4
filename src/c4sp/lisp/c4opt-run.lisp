;; M6 driver: optimize a .c4r image.
;;
;;   c4sp c4opt-run.lisp in.c4r [out.c4r]
;;
;; Decodes, runs the c4opt passes to a fixpoint, prints what happened,
;; and (native only) writes the optimized image.
(begin
	(load "c4r.lisp")
	(load "c4opt.lisp")
	(if (empty? argv) (error "usage: c4opt-run.lisp in.c4r [out.c4r]"))
	(define In (head argv))
	(define Orig (file:read (file:path In)))
	(define M (c4r:decode Orig))
	(define M2 (c4opt:optimize M))
	(define Out (c4r:encode M2))
	(print ";; c4opt:" In "-" (length Orig) "->" (length Out) "bytes")
	(if (not (empty? (tail argv)))
		(if (file:write (index argv 1) Out)
			(print ";; wrote" (index argv 1))
			(error "c4opt: write failed"))))
