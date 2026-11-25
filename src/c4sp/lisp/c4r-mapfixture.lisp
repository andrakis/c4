;; c4r-mapfixture.lisp -- test-srcmap's fixture (docs/c4-explorer.md, S0)
;;
;;   c4sp c4r-mapfixture.lisp in.c4r out.c4r
;;
;; The image with an extension trailer added: a source map made by hand,
;; then a chunk with a lower-case tag. Every loader must run the result
;; as it runs the image, c4rdump -l must read the map back, c4opt must
;; keep the second chunk and drop the map (an upper-case tag describes
;; the code as it was), and c4rstrip must drop both.
(begin
	(load "c4r.lisp")
	(if (< (length argv) 2) (error "usage: c4r-mapfixture.lisp in.c4r out.c4r"))
	(define poke (lambda (L S I)
		(if (empty? L) S
		(begin (string:byte! S I (head L)) (next poke (tail L) S (+ I 1))))))
	(define bytes (lambda (L) (poke L (string:alloc (length L)) 0)))
	(define Map (bytes (list
		1 1                                 ;; version 1, files carry a CRC
		1 9 102 105 120 116 117 114 101 46 99    ;; one file, "fixture.c"
		163 2                               ;; of 291 bytes
		239 190 173 222                     ;; CRC-32 0xdeadbeef, low first
		1 1 29 4 109 97 105 110 0 3         ;; one function: word 1, 29 words, "main", file 0 line 3
		2 3 0 3 20 2)))                     ;; two rows: word 1 file 0 line 3; word 11 line 5
	(define M (c4r:decode (file:read (file:path (head argv)))))
	(set! c4r:extra-chunks (list (list "SMAP" Map) (list "note" "a chunk c4opt keeps")))
	(define Out (c4r:encode M))
	(set! c4r:extra-chunks (list))
	(if (file:write (index argv 1) Out)
		(print ";; c4r-mapfixture:" (head argv) "->" (index argv 1) "-" (length Out) "bytes")
		(error "c4r-mapfixture: write failed")))
