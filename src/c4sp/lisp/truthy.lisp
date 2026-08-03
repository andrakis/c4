;; Truthiness regression: false, nil/() and integer 0 are falsy; the
;; atom nil counts as nil. Everything else -- including 0.0 and "" --
;; is truthy (Common Lisp style). Deliberate divergence from alisp,
;; where only false is false. (not x) must agree with (if x ...).
(begin
	(define show (lambda (name x)
		(print name (if x "truthy" "falsy") (not x))))
	(show "false" false)
	(show "0    " 0)
	(show "nil  " nil)
	(show "()   " (list))
	(show "'nil " 'nil)
	(show "true " true)
	(show "1    " 1)
	(show "-1   " (- 0 1))
	(show "0.0  " 0.0)
	(show "str0 " "")
	(show "str  " "a")
	(show "atom " 'x)
	(show "list " (list 1))
	(show "lmbda" show))
