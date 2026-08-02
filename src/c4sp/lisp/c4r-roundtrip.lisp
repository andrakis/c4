;; M5 acceptance test: foo.c4r -> lists -> foo.c4r must be byte-identical.
;;
;;   c4sp c4r-roundtrip.lisp some.c4r [out.c4r]
;;
;; Decodes the image into labelled instruction lists, re-encodes, and
;; compares the bytes in memory (so this also runs under the C4 VM, which
;; has no write syscall). With a second argument the re-encoded image is
;; written out too (native only).
(begin
	(load "c4r.lisp")
	(if (empty? argv) (error "usage: c4r-roundtrip.lisp file.c4r [out.c4r]"))
	(define In (head argv))
	(define Orig (file:read (file:path In)))
	(define M (c4r:decode Orig))
	(define Out (c4r:encode M))
	(if (= Orig Out)
		(print "roundtrip identical:" In "-" (length Orig) "bytes,"
			(length (index M 3)) "instructions")
		(begin
			(print "ROUNDTRIP DIFFERS:" In "-" (length Orig) "->" (length Out) "bytes")
			(error "roundtrip failed")))
	(if (not (empty? (tail argv)))
		(file:write (index argv 1) Out)))
