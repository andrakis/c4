(begin
	;; Macro that rewrites (?def symbol) to (env:defined (quote symbol)).
	;; Makes it so one does not need to (quote) their symbol name.
	(define ?def (fastmacro (sym) (list (quote env:defined) 
		(list (quote quote) sym))))

	;; tail recursive factorial
	(define fac (lambda (n) (fac/2 n 1)))
	(define fac/2 (lambda (n a)
		(if (= 1 n) a (next fac/2 (- n 1) (* n a)))))

	(define x 10) ;; default value
	(if (?def argv)
		(if (not (empty? argv)) (begin
			(define h (head argv))
			(print "Using" h "for factorial")
			(set! x h))))
	(print "Factorial of" x "=" (fac x))
)
