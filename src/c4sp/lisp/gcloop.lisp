;; M2 acceptance test: a loop allocating millions of cells must run in a
;; fixed arena. Each iteration allocates a fresh list and env bindings; the
;; previous iteration's garbage must be collected or the default 64K-cell
;; arena dies within a few thousand iterations.
(begin
	(define loop (lambda (n)
		(begin
			(define junk (list n n n n (list n n n)))
			(if (= 0 n)
				"done"
				(next loop (- n 1))))))
	;; Iteration count from argv when given (the interpreted-c4m test run
	;; uses fewer), else a million.
	(define count 1000000)
	(if (env:defined 'argv)
		(if (not (empty? argv))
			(set! count (head argv))))
	(print (loop count))
	(print "gc loop survived a fixed arena"))
