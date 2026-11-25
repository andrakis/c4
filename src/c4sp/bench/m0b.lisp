;; M0 kernel B: the other extreme -- dominated by USER function calls
;; (two non-tail calls per node) rather than by builtins. Kernel A is
;; the best case for compilation; this is the worst, and c4lc lives
;; between them (24% of its applications are user functions).
(begin
	(define fib (lambda (n)
		(if (<= n 1) n (+ (fib (- n 1)) (fib (- n 2))))))
	(fib 30))
