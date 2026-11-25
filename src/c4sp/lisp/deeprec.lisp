;; M3.5 acceptance test: non-tail recursion 4000 deep (deeper than the C4 VM stack can hold eval frames). The recursive
;; evaluator burns a C4 stack frame per level and dies long before this;
;; the CEK machine keeps continuations in the arena and does not care.
(begin
	(define depth (lambda (n)
		(if (= 0 n) 0 (+ 1 (depth (- n 1))))))
	(print "depth:" (depth 4000)))
