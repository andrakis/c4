;; M6 driver: optimize a .c4r image.
;;
;;   c4sp c4opt-run.lisp [-mfuse] in.c4r [out.c4r]
;;
;; Decodes, runs the c4opt passes to a fixpoint, prints what happened,
;; and (native only) writes the optimized image.
;;
;; -mfuse additionally runs the fuse pass (docs/fused-opcodes.md), which
;; is how an image from ANY compiler -- c4cc's included -- gets the fused
;; opcodes. The result runs under c4m, c4mp and oisc4 but NOT under plain
;; c4, which is why it is opt-in.
(begin
	(load "c4r.lisp")
	(load "c4opt.lisp")
	(if (empty? argv) (error "usage: c4opt-run.lisp [-mfuse] in.c4r [out.c4r]"))
	(if (= (+ "" (head argv)) "-mfuse")
		(begin (set! opt:fuse-on true) (set! argv (tail argv)))
		nil)
	(if (empty? argv) (error "usage: c4opt-run.lisp [-mfuse] in.c4r [out.c4r]"))
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
