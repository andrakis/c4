;; call/cc: continuations are the CEK machine's kont chain, captured by
;; copying one pointer (frames are immutable). alisp has no equivalent, so
;; this file has no Node oracle; expected output is verified by hand.
(begin
	;; no escape: the value returns normally
	(print (call/cc (lambda (k) 42)))
	;; escape: (k 20) abandons (+ 10 _) entirely
	(print (+ 1 (call/cc (lambda (k) (+ 10 (k 20))))))
	;; escape from deep recursion: product of a list, bailing on 0
	(define product (lambda (l k)
		(if (empty? l) 1
			(if (= 0 (head l)) (k 0)
				(* (head l) (product (tail l) k))))))
	(print (call/cc (lambda (k) (product (list 1 2 3 4 5) k))))
	(print (call/cc (lambda (k) (product (list 1 2 0 4 5) k))))
	;; a stored continuation re-enters its capture point each invocation
	(define resume nil)
	(define n (call/cc (lambda (k) (begin (set! resume k) 0))))
	(print "n =" n)
	(if (< n 3) (resume (+ n 1)))
	(print "done, n =" n)
	;; a continuation is first class
	(print (typeof resume)))
